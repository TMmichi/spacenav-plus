#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "spnav.h"

#ifdef USE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Window get_daemon_window(Display *dpy);
static int catch_badwin(Display *dpy, XErrorEvent *err);

static Display *dpy;
static Window app_win;
static Atom motion_event, button_press_event, button_release_event, command_event;

enum {
	CMD_APP_WINDOW = 27695,
	CMD_APP_SENS
};

#define IS_OPEN		(dpy || sock)
#else
#define IS_OPEN		(sock)
#endif

static int sock;

int spnav_open(void)
{
	int s;
	struct sockaddr_un addr;

	if(IS_OPEN) {
		return -1;
	}

	if((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		return -1;
	}

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, "/tmp/spacenav_usock");

	if(connect(s, (struct sockaddr*)&addr, sizeof addr) == -1) {
		return -1;
	}

	sock = s;
	return 0;
}

#ifdef USE_X11
int spnav_x11_open(Display *display, Window win)
{
	if(IS_OPEN) {
		return -1;
	}

	dpy = display;

	motion_event = XInternAtom(dpy, "MotionEvent", True);
	button_press_event = XInternAtom(dpy, "ButtonPressEvent", True);
	button_release_event = XInternAtom(dpy, "ButtonReleaseEvent", True);
	command_event = XInternAtom(dpy, "CommandEvent", True);

	if(!motion_event || !button_press_event || !button_release_event || !command_event) {
		dpy = 0;
		return -1;	/* daemon not started */
	}

	if(spnav_x11_window(win) == -1) {
		dpy = 0;
		return -1;	/* daemon not started */
	}

	app_win = win;
	return 0;
}
#endif

int spnav_close(void)
{
	if(!IS_OPEN) {
		return -1;
	}

	if(sock) {
		close(sock);
		sock = 0;
		return 0;
	}

#ifdef USE_X11
	if(dpy) {
		spnav_x11_window(DefaultRootWindow(dpy));
		app_win = 0;
		dpy = 0;
		return 0;
	}
#endif

	return -1;
}


#ifdef USE_X11
int spnav_x11_window(Window win)
{
	int (*prev_xerr_handler)(Display*, XErrorEvent*);
	XEvent xev;
	Window daemon_win;

	if(!IS_OPEN) {
		return -1;
	}

	if(!(daemon_win = get_daemon_window(dpy))) {
		return -1;
	}

	prev_xerr_handler = XSetErrorHandler(catch_badwin);

	xev.type = ClientMessage;
	xev.xclient.send_event = False;
	xev.xclient.display = dpy;
	xev.xclient.window = win;
	xev.xclient.message_type = command_event;
	xev.xclient.format = 16;
	xev.xclient.data.s[0] = ((unsigned int)win & 0xffff0000) >> 16;
	xev.xclient.data.s[1] = (unsigned int)win & 0xffff;
	xev.xclient.data.s[2] = CMD_APP_WINDOW;

	XSendEvent(dpy, daemon_win, False, 0, &xev);
	XSync(dpy, False);

	XSetErrorHandler(prev_xerr_handler);
	return 0;
}

static int x11_sensitivity(double sens)
{
	int (*prev_xerr_handler)(Display*, XErrorEvent*);
	XEvent xev;
	Window daemon_win;
	float fsens;
	unsigned int isens;
	
	if(!(daemon_win = get_daemon_window(dpy))) {
		return -1;
	}

	fsens = sens;
	isens = *(unsigned int*)&fsens;

	prev_xerr_handler = XSetErrorHandler(catch_badwin);

	xev.type = ClientMessage;
	xev.xclient.send_event = False;
	xev.xclient.display = dpy;
	xev.xclient.window = app_win;
	xev.xclient.message_type = command_event;
	xev.xclient.format = 16;
	xev.xclient.data.s[0] = isens & 0xffff;
	xev.xclient.data.s[1] = (isens & 0xffff0000) >> 16;
	xev.xclient.data.s[2] = CMD_APP_SENS;

	XSendEvent(dpy, daemon_win, False, 0, &xev);
	XSync(dpy, False);

	XSetErrorHandler(prev_xerr_handler);
	return 0;
}
#endif

int spnav_sensitivity(double sens)
{
#ifdef USE_X11
	if(dpy) {
		return x11_sensitivity(sens);
	}
#endif

	if(sock) {
		ssize_t bytes;
		float fval = sens;

		while((bytes = write(sock, &fval, sizeof fval)) <= 0 && errno == EINTR);
		if(bytes <= 0) {
			return -1;
		}
		return 0;
	}

	return -1;
}

int spnav_fd(void)
{
#ifdef USE_X11
	if(dpy) {
		return ConnectionNumber(dpy);
	}
#endif

	return sock ? sock : -1;
}


int spnav_wait_event(spnav_event *event)
{
#ifdef USE_X11
	if(dpy) {
		for(;;) {
			int evtype;
			XEvent xev;
			XNextEvent(dpy, &xev);

			if((evtype = spnav_x11_event(&xev, event)) > 0) {
				return evtype;
			}
		}
	}
#endif

	if(sock) {
		/* TODO implement event reception from the UNIX socket */
	}
	return 0;
}

int spnav_poll_event(spnav_event *event)
{
#ifdef USE_X11
	if(dpy) {
		if(XPending(dpy)) {
			XEvent xev;
			XNextEvent(dpy, &xev);

			return spnav_x11_event(&xev, event);
		}
		return 0;
	}
#endif

	if(sock) {
		/* TODO implement event reception from the UNIX socket */
	}
	return 0;
}

#ifdef USE_X11
static Bool match_events(Display *dpy, XEvent *xev, char *arg)
{
	int evtype = *(int*)arg;

	if(xev->type != ClientMessage) {
		return False;
	}

	if(xev->xclient.message_type == motion_event) {
		return !evtype || evtype == SPNAV_EVENT_MOTION ? True : False;
	}
	if(xev->xclient.message_type == button_press_event ||
			xev->xclient.message_type == button_release_event) {
		return !evtype || evtype == SPNAV_EVENT_BUTTON ? True : False;
	}
	return False;
}
#endif

int spnav_remove_events(int type)
{
	int rm_count = 0;

#ifdef USE_X11
	if(dpy) {
		XEvent xev;

		while(XCheckIfEvent(dpy, &xev, match_events, (char*)&type)) {
			rm_count++;
		}
		return rm_count;
	}
#endif

	if(sock) {
		/* TODO */
	}
	return 0;
}

#ifdef USE_X11
int spnav_x11_event(const XEvent *xev, spnav_event *event)
{
	int i;
	int xmsg_type;

	if(xev->type != ClientMessage) {
		return 0;
	}

	xmsg_type = xev->xclient.message_type;

	if(xmsg_type != motion_event && xmsg_type != button_press_event &&
			xmsg_type != button_release_event) {
		return 0;
	}

	if(xmsg_type == motion_event) {
		event->type = SPNAV_EVENT_MOTION;
		event->motion.data = &event->motion.x;

		for(i=0; i<6; i++) {
			event->motion.data[i] = xev->xclient.data.s[i + 2];
		}
		event->motion.period = xev->xclient.data.s[8];
	} else {
		event->type = SPNAV_EVENT_BUTTON;
		event->button.press = xmsg_type == button_press_event ? 1 : 0;
		event->button.bnum = xev->xclient.data.s[2];
	}
	return event->type;
}


static Window get_daemon_window(Display *dpy)
{
	Window win, root_win;
	XTextProperty wname;
	Atom type;
	int fmt;
	unsigned long nitems, bytes_after;
	unsigned char *prop;

	root_win = DefaultRootWindow(dpy);

	XGetWindowProperty(dpy, root_win, command_event, 0, 1, False, AnyPropertyType, &type, &fmt, &nitems, &bytes_after, &prop);
	if(!prop) {
		return 0;
	}

	win = *(Window*)prop;
	XFree(prop);

	if(!XGetWMName(dpy, win, &wname) || strcmp("Magellan Window", (char*)wname.value) != 0) {
		return 0;
	}

	return win;
}

int catch_badwin(Display *dpy, XErrorEvent *err)
{
	char buf[256];

	if(err->error_code == BadWindow) {
		/* do nothing? */
	} else {
		XGetErrorText(dpy, err->error_code, buf, sizeof buf);
		fprintf(stderr, "Caught unexpected X error: %s\n", buf);
	}
	return 0;
}
#endif
