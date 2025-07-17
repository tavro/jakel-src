#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define JAKEL_VERSION "0.0.1"

#define K_CTRL(k) ((k) & 0x1f)

enum keys {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,

	DELETE,

	HOME,
	END,

	PAGE_UP,
	PAGE_DOWN
};

struct config {
	int cx, cy;

	int rows;
	int cols;

	struct termios ot;
};

struct config C;

void kill(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void exitRawMode() {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &C.ot) == -1) kill("tcsetattr");
}

void enterRawMode() {
	if (tcgetattr(STDIN_FILENO, &C.ot) == -1) kill("tcgetattr");

	atexit(exitRawMode);

	struct termios rt = C.ot;

	// Input flags
	// -----------
	// IXON: Disables software flow control (CTRL-S/CTRL-Q).
	//       We turn this off to prevent the terminal from pausing/resuming input.
	// ICRNL: Disables automatic conversion of carriage return ('\r') to newline ('\n').
	//        This ensures CTRL-M and ENTER are treated the same.
	// BRKINT: Disables generation of SIGINT on a break condition.
	//         We don't want the program to be interrupted this way.
	// INPCK: Disables parity checking (not needed for most modern systems).
	// ISTRIP: Disables stripping of the 8th bit of input bytes.
	//         We want to preserve all 8 bits.
	rt.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	// Output flags
	// ------------
	// OPOST: Disables output processing (e.g., converting '\n' to '\r\n').
	//        In raw mode, we want to output data exactly as-is.
	rt.c_oflag &= ~(OPOST);

	// Control flags
	// -------------
	// CS8: Sets character size to 8 bits per byte.
	//      This is a bit mask, not a flag to disable.
	rt.c_cflag |= (CS8);

	// Local flags
	// -----------
	// ECHO: Disables echoing of input characters to the terminal.
	//       Necessary for raw input mode.
	// ICANON: Disables canonical mode, allowing input to be read byte by byte
	//         instead of line by line.
	// ISIG: Disables signal generation for special characters like CTRL-C (SIGINT)
	//       and CTRL-Z (SIGTSTP).
	// IEXTEN: Disables implementation defined input processing (e.g., CTRL-V).
	rt.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	rt.c_cc[VMIN] = 0;
	rt.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rt) == -1) kill("tcsetattr");
}

int readKey() {
	int n;
	char c;

	while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
		if (n == -1 && errno != EAGAIN) kill("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '-') {
					switch (seq[1]) {
						case '1': return HOME;
						case '3': return DELETE;
						case '4': return END;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME;
						case '8': return END;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME;
					case 'F': return END;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME;
				case 'F': return END;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPos(int *rs, int *cs) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rs, cs) != 2) return -1;

	return 0;
}

int getSize(int *rs, int *cs) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPos(rs, cs);
	} else {
		*cs = ws.ws_col;
		*rs = ws.ws_row;
		return 0;
	}
}

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0};

void appendToBuf(struct abuf *ab, const char *s, int len) {
	char *nc = realloc(ab->b, ab->len + len);

	if (nc == NULL) return;
	memcpy(&nc[ab->len], s, len);
	ab->b = nc;
	ab->len += len;
}

void freeBuf(struct abuf *ab) {
	free(ab->b);
}

void moveCursor(char c) {
	switch (c) {
		case ARROW_UP:
			if (C.cy != 0) {
				C.cy--;
			}
			break;
		case ARROW_LEFT:
			if (C.cx != 0) {
				C.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (C.cx != C.cols - 1) {
				C.cx++;
			}
			break;
		case ARROW_DOWN:
			if (C.cy != C.rows - 1) {
				C.cy++;
			}
			break;
	}
}

void processKey() {
	int c = readKey();

	switch (c) {
		case K_CTRL('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
        		write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case HOME:
			C.cx = 0;
			break;
		case END:
			C.cx = C.cols - 1;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int t = C.rows;
				while (t--) moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			moveCursor(c);
			break;
	}
}

void drawRowIndicators(struct abuf *ab) {
	int row;
	for (row = 0; row < C.rows; row++) {
		if (row == C.rows / 3) {
			char headerMsg[80];
			int headerMsgLen = snprintf(headerMsg, sizeof(headerMsg), "jakel -- version %s", JAKEL_VERSION);

			if (headerMsgLen > C.cols) headerMsgLen = C.cols;

			int padding = (C.cols - headerMsgLen) / 2;
			if (padding) {
				appendToBuf(ab, "-", 1);
				padding--;
			}
			while (padding--) appendToBuf(ab, " ", 1);

			appendToBuf(ab, headerMsg, headerMsgLen);
		} else {
			appendToBuf(ab, "-", 1);
		}

		appendToBuf(ab, "\x1b[K", 3);
		if (row < C.rows - 1) {
			appendToBuf(ab, "\r\n", 2);
		}
	}
}

void clear() {
	struct abuf ab = ABUF_INIT;

	appendToBuf(&ab, "\x1b[?25l", 6);
	appendToBuf(&ab, "\x1b[H", 3);

	drawRowIndicators(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", C.cy + 1, C.cx + 1);
	appendToBuf(&ab, buf, strlen(buf));

	appendToBuf(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	freeBuf(&ab);
}

void init() {
	C.cx = 0;
	C.cy = 0;

	if (getSize(&C.rows, &C.cols) == -1) kill("getSize");
}

int main() {
	enterRawMode();
	init();

	while (1) {
		clear();
		processKey();
	}

	return 0;
}

