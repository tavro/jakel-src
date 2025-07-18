#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define JAKEL_VERSION "0.0.1"
#define JAKEL_TAB_STOP 8

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

typedef struct line {
	int size;
	int rsize;

	char *chars;
	char *render;
} line;

struct config {
	int cx, cy;
	int rx;

	int rowOffset;
	int colOffset;

	int rows;
	int cols;

	int rowAmount;
	line *row;

	char *filename; // TODO: Update name

	char statusMsg[80];
	time_t statusMsgTime;

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

int rowCxToRx(line *row, int cx) {
	int rx = 0;
	int j;
	for ( j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			rx += (JAKEL_TAB_STOP - 1) - (rx % JAKEL_TAB_STOP);
		}
		rx++;
	}

	return rx;
}

void updateRow(line *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(JAKEL_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % JAKEL_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}

	row->render[idx] = '\0';
	row->rsize = idx;
}

void appendRow(char *s, size_t len) {
	C.row = realloc(C.row, sizeof(line) * (C.rowAmount + 1));

	int at = C.rowAmount;
	C.row[at].size = len;
	C.row[at].chars = malloc(len + 1);

	memcpy(C.row[at].chars, s, len);

	C.row[at].chars[len] = '\0';

	C.row[at].rsize = 0;
	C.row[at].render = NULL;
	updateRow(&C.row[at]);

	C.rowAmount++;
}


void open(char *filename) {
	free(C.filename);
	C.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) kill("fopen");

	char *l = NULL;
	size_t lcap = 0;
	ssize_t llen;

	while((llen = getline(&l, &lcap, fp)) != -1) {
		while (llen > 0 && (l[llen - 1] == '\n' || l[llen - 1] == '\r')) llen--;
		appendRow(l, llen);
	}

	free(l);
	fclose(fp);
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

void moveCursor(int c) {
	line *row = (C.cy >= C.rowAmount) ? NULL : &C.row[C.cy];

	switch (c) {
		case ARROW_UP:
			if (C.cy != 0) {
				C.cy--;
			}
			break;
		case ARROW_LEFT:
			if (C.cx != 0) {
				C.cx--;
			} else if (C.cy > 0) {
				C.cy--; // IDEA: Stay on same line instead?
				C.cx = C.row[C.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && C.cx < row->size) {
				C.cx++;
			} else if (row && C.cx == row->size) {
				C.cy++; // IDEA: Stay on same line instead?
				C.cx = 0;
			}
			break;
		case ARROW_DOWN:
			if (C.cy < C.rowAmount) {
				C.cy++;
			}
			break;
	}

	row = (C.cy >= C.rowAmount) ? NULL : &C.row[C.cy];
	int rowlen = row ? row->size : 0;
	if (C.cx > rowlen) {
		C.cx = rowlen;
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
			if (C.cy < C.rowAmount) C.cx = C.row[C.cy].size;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					C.cy = C.rowOffset;
				} else if (c == PAGE_DOWN) {
					C.cy = C.rowOffset + C.rows - 1;
					if (C.cy > C.rowAmount) C.cy = C.rowAmount;
				}

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

void scroll() {
	C.rx = 0;
	if (C.cy < C.rowAmount) {
		C.rx = rowCxToRx(&C.row[C.cy], C.cx);
	}

	if (C.cy < C.rowOffset) {
		C.rowOffset = C.cy;
	}

	if (C.cy >= C.rowOffset + C.rows) {
		C.rowOffset = C.cy - C.rows + 1;
	}

	if (C.rx < C.colOffset) {
		C.colOffset = C.rx;
	}

	if (C.rx >= C.colOffset + C.cols) {
		C.colOffset = C.rx - C.cols + 1;
	}
}

void draw(struct abuf *ab) {
	int row;
	for (row = 0; row < C.rows; row++) {
		int fileRow = row + C.rowOffset;
		if (fileRow >= C.rowAmount) {
			if (C.rowAmount == 0 && row == C.rows / 3) {
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
		} else {
			int len = C.row[fileRow].rsize - C.colOffset;
			if (len < 0) len = 0;
			if (len > C.cols) len = C.cols;
			appendToBuf(ab, &C.row[fileRow].render[C.colOffset], len);
		}

		appendToBuf(ab, "\x1b[K", 3);
		appendToBuf(ab, "\r\n", 2);
	}
}

void drawStatusBar(struct abuf *ab) {
	appendToBuf(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines", C.filename ? C.filename : "[Unnamed]", C.rowAmount);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", C.cy + 1, C.rowAmount);

	if (len > C.cols) len = C.cols;
	appendToBuf(ab, status, len);

	while (len < C.cols) {
		if (C.cols - len == rlen) {
			appendToBuf(ab, rstatus, rlen);
			break;
		} else {
			appendToBuf(ab, " ", 1);
			len++;
		}
	}

	appendToBuf(ab, "\x1b[m", 3);
	appendToBuf(ab, "\r\n", 2);
}

void drawMsgBar(struct abuf *ab) {
	appendToBuf(ab, "\x1b[K", 3);

	int msgLen = strlen(C.statusMsg);
	if (msgLen > C.cols) msgLen = C.cols;
	if (msgLen && time(NULL) - C.statusMsgTime < 5) appendToBuf(ab, C.statusMsg, msgLen);
}

void clear() {
	scroll();

	struct abuf ab = ABUF_INIT;

	appendToBuf(&ab, "\x1b[?25l", 6);
	appendToBuf(&ab, "\x1b[H", 3);

	draw(&ab);
	drawStatusBar(&ab);
	drawMsgBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (C.cy - C.rowOffset) + 1, (C.rx - C.colOffset) + 1);
	appendToBuf(&ab, buf, strlen(buf));

	appendToBuf(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	freeBuf(&ab);
}

void setStatusMsg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(C.statusMsg, sizeof(C.statusMsg), fmt, ap);
	va_end(ap);
	C.statusMsgTime = time(NULL);
}

void init() {
	C.cx = 0;
	C.cy = 0;
	C.rx = 0;
	C.rowOffset = 0;
	C.colOffset = 0;
	C.rowAmount = 0;
	C.row = NULL;
	C.filename = NULL;
	C.statusMsg[0] = '\0';
	C.statusMsgTime = 0;

	if (getSize(&C.rows, &C.cols) == -1) kill("getSize");

	C.rows -= 2;
}

int main(int argc, char *argv[]) {
	enterRawMode();
	init();

	if (argc >= 2) {
		open(argv[1]);
	}

	setStatusMsg("CTRL-Q = quit");

	while (1) {
		clear();
		processKey();
	}

	return 0;
}

