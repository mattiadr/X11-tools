/*
	Compile with:
	`gcc winfocus.c -Wall -lX11 -o winfocus`
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define HELP "Usage: %s {up|down|left|right} [min_distance=%d]\n"
#define MAX_PROPERTY_VALUE_LEN 4096
#define MIN_DIST_DEFAULT 50

char * get_property(Display *disp, Window win, Atom xa_prop_type, char *prop_name, unsigned long *size);
Window * get_client_list(Display *disp, unsigned long *size);
Window get_active_client(Display *disp);
Bool client_has_state(Display *disp, Window win, char *atom_name);
Window * filter_clients(Display *disp, Window *clients, unsigned long *num_clients, Window active_client);
void get_client_pos(Display *disp, Window win, int *center_x, int *center_y);
Window get_closest_client(Display *disp, Window active_client, Window *client_list, unsigned long num_clients, char *direction, int min_dist);
void activate_client(Display *disp, Window win);

int main(int argc, char *argv[]) {
	Display *disp;
	Window *clients;
	Window active_client, closest_client;
	unsigned long num_clients;
	int min_dist;

	if (argc < 2 || (argv[1][0] != 'u' && argv[1][0] != 'd' && argv[1][0] != 'l' && argv[1][0] != 'r')) {
		printf(HELP, argv[0], MIN_DIST_DEFAULT);
		return EXIT_SUCCESS;
	}

	if (argc >= 3) {
		min_dist = atoi(argv[2]);
	} else {
		min_dist = 50;
	}

	if (!(disp = XOpenDisplay(NULL))) {
		fprintf(stderr, "Cannot open display.\n");
		return EXIT_FAILURE;
	}

	clients = get_client_list(disp, &num_clients);
	active_client = get_active_client(disp);
	clients = filter_clients(disp, clients, &num_clients, active_client);
	closest_client = get_closest_client(disp, active_client, clients, num_clients, argv[1], min_dist);

	if (closest_client) {
		activate_client(disp, closest_client);
	}

	XCloseDisplay(disp);
}

char * get_property(Display *disp, Window win, Atom xa_prop_type, char *prop_name, unsigned long *size) {
	Atom xa_prop_name;
	Atom xa_ret_type;
	int ret_format;
	unsigned long ret_nitems;
	unsigned long ret_bytes_after;
	unsigned long tmp_size;
	unsigned char *ret_prop;
	char *ret;

	xa_prop_name = XInternAtom(disp, prop_name, False);

	/* MAX_PROPERTY_VALUE_LEN / 4 explanation (XGetWindowProperty manpage):
	 *
	 * long_length = Specifies the length in 32-bit multiples of the
	 *               data to be retrieved.
	 *
	 * NOTE:  see
	 * http://mail.gnome.org/archives/wm-spec-list/2003-March/msg00067.html
	 * In particular:
	 *
	 * 	When the X window system was ported to 64-bit architectures, a
	 * rather peculiar design decision was made. 32-bit quantities such
	 * as Window IDs, atoms, etc, were kept as longs in the client side
	 * APIs, even when long was changed to 64 bits.
	 *
	 */
	if (XGetWindowProperty(disp, win, xa_prop_name, 0, MAX_PROPERTY_VALUE_LEN / 4, False, xa_prop_type, &xa_ret_type, &ret_format,
						   &ret_nitems, &ret_bytes_after, &ret_prop) != Success) {
		fprintf(stderr, "Cannot get %s property.\n", prop_name);
		return NULL;
	}

	if (xa_ret_type != xa_prop_type) {
		fprintf(stderr, "Invalid type of %s property.\n", prop_name);
		XFree(ret_prop);
		return NULL;
	}

	/* null terminate the result to make string handling easier */

	tmp_size = (ret_format / 8) * ret_nitems;
	/* Correct 64 Architecture implementation of 32 bit data */
	if (ret_format == 32) tmp_size *= sizeof(long)/4;
	ret = malloc(tmp_size + 1);
	memcpy(ret, ret_prop, tmp_size);
	ret[tmp_size] = '\0';

	if (size) {
		*size = tmp_size;
	}

	XFree(ret_prop);
	return ret;
}

Window * get_client_list(Display *disp, unsigned long *num_clients) {
	Window *client_list = NULL;
	unsigned long size;

	client_list = (Window *) get_property(disp, DefaultRootWindow(disp), XA_WINDOW, "_NET_CLIENT_LIST", &size);

	if (!client_list) {
		client_list = (Window *) get_property(disp, DefaultRootWindow(disp), XA_CARDINAL, "_WIN_CLIENT_LIST", &size);
	}

	if (!client_list) {
		fprintf(stderr, "Cannot get client list properties (_NET_CLIENT_LIST or _WIN_CLIENT_LIST).\n");
	}

	*num_clients = size / sizeof(Window);
	return client_list;
}

Window get_active_client(Display *disp) {
	unsigned long size;
	Window *win;
	Window ret = (Window) 0;

	win = (Window *) get_property(disp, DefaultRootWindow(disp), XA_WINDOW, "_NET_ACTIVE_WINDOW", &size);
	if (win) {
		ret = *win;
		free(win);
	}

	return ret;
}

Bool client_has_state(Display *disp, Window win, char *atom_name) {
	Atom atom;
	unsigned long size;
	Atom *list;

	atom = XInternAtom(disp, atom_name, False);
	if (!(list = (Atom *) get_property(disp, win, XA_ATOM, "_NET_WM_STATE", &size))) {
		fprintf(stderr, "Cannot get _NET_WM_STATE property.\n");
		return False;
	}

	for (int i = 0; i < size / sizeof(Atom); i++) {
		if (list[i] == atom) {
			free(list);
			return True;
		}
	}

	free(list);
	return False;
}

Window * filter_clients(Display *disp, Window *clients, unsigned long *num_clients, Window active_client) {
	Window root = DefaultRootWindow(disp);
	Window *ret = malloc(*num_clients);
	unsigned long num_ret = 0;
	unsigned long *cur_desktop;
	unsigned long *desktop;

	/* get current desktop */
	if (!(cur_desktop = (unsigned long *) get_property(disp, root, XA_CARDINAL, "_NET_CURRENT_DESKTOP", NULL))) {
		if (!(cur_desktop = (unsigned long *) get_property(disp, root, XA_CARDINAL, "_WIN_WORKSPACE", NULL))) {
			fprintf(stderr, "Cannot get current desktop properties (_NET_CURRENT_DESKTOP or _WIN_WORKSPACE property).\n");
			return NULL;
		}
	}

	for (int i = 0; i < *num_clients; i++) {
		/* skip active client */
		if (clients[i] == active_client) {
			continue;
		}
		/* skip SKIP_PAGER */
		if (client_has_state(disp, clients[i], "_NET_WM_STATE_SKIP_PAGER")) {
			continue;
		}
		/* get client desktop */
		if (!(desktop = (unsigned long *) get_property(disp, clients[i], XA_CARDINAL, "_NET_WM_DESKTOP", NULL))) {
			desktop = (unsigned long *) get_property(disp, clients[i], XA_CARDINAL, "_WIN_WORKSPACE", NULL);
		}
		/* copy if has valid desktop and is on the current/all desktops */
		if (desktop && (*desktop == -1 || *desktop == *cur_desktop)) {
			ret[num_ret++] = clients[i];
		}
	}

	*num_clients = num_ret;

	free(cur_desktop);
	free(desktop);
	free(clients);
	return ret;
}

void get_client_pos(Display *disp, Window win, int *center_x, int *center_y) {
	Window junkroot;
	int x, y, junkx, junky;
	unsigned int width, height, bw, depth;

	XGetGeometry(disp, win, &junkroot, &junkx, &junky, &width, &height, &bw, &depth);
	XTranslateCoordinates(disp, win, junkroot, junkx, junky, &x, &y, &junkroot);

	*center_x = x + width / 2;
	*center_y = y + height / 2;
}

int sign(int x) {
	if (x > 0) {
		return +1;
	} else if (x < 0) {
		return -1;
	} else {
		return 0;
	}
}

Window get_closest_client(Display *disp, Window active_client, Window *client_list, unsigned long num_clients, char *direction, int min_dist) {
	Window closest = (Window) 0;
	unsigned int closest_distance = UINT_MAX;
	int active_x, active_y, x, y, dist_x, dist_y, dist;
	int dir_x = 0, dir_y = 0;

	switch (direction[0]) {
	case 'u':
		dir_y = -1;
		break;
	case 'd':
		dir_y = +1;
		break;
	case 'l':
		dir_x = -1;
		break;
	case 'r':
		dir_x = +1;
		break;
	default:
		return (Window) 0;
	}

	get_client_pos(disp, active_client, &active_x, &active_y);

	for (int i = 0; i < num_clients; i++) {
		get_client_pos(disp, client_list[i], &x, &y);
		dist_x = x - active_x;
		dist_y = y - active_y;
		if (abs(dist_x) < min_dist) dist_x = 0;
		if (abs(dist_y) < min_dist) dist_y = 0;
		/* check if the current client is in the specified direction */
		if ((dir_x == 0 || sign(dist_x) == dir_x) && (dir_y == 0 || sign(dist_y) == dir_y)) {
			/* get closest client */
			dist = abs(dist_x) + abs(dist_y);
			if (dist < closest_distance) {
				closest_distance = dist;
				closest = client_list[i];
			}
		}
	}

	return closest;
}

void activate_client(Display *disp, Window win) {
	XEvent event;
	long mask = SubstructureRedirectMask | SubstructureNotifyMask;

	event.xclient.type = ClientMessage;
	event.xclient.serial = 0;
	event.xclient.send_event = True;
	event.xclient.message_type = XInternAtom(disp, "_NET_ACTIVE_WINDOW", False);
	event.xclient.window = win;
	event.xclient.format = 32;

	if (!XSendEvent(disp, DefaultRootWindow(disp), False, mask, &event)) {
		fprintf(stderr, "Cannot send _NET_ACTIVE_WINDOW event.\n");
	}

	XMapRaised(disp, win);
}
