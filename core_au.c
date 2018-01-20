/* radare - LGPL - Copyright 2018 - pancake */
#if 0
gcc -o core_au.so -fPIC `pkg-config --cflags --libs r_core` core_test.c -shared
mkdir -p ~/.config/radare2/plugins
mv core_au.so ~/.config/radare2/plugins
#endif

#include <r_types.h>
#include <r_lib.h>
#include <r_cmd.h>
#include <r_core.h>
#include <string.h>
#include "ao.h"
#define _USE_MATH_DEFINES
#include <math.h>

#undef R_API
#define R_API static
#undef R_IPI
#define R_IPI static

static int waveType = 0;
static int waveFreq = 500;
static int printMode = 0;

enum {
	FORM_SIN,      // .''.''.
	FORM_COS,      // '..'..'
	FORM_SAW,      // /|/|/|/
	FORM_PULSE,    // |_|'|_|
	FORM_NOISE,    // \:./|.:
	FORM_TRIANGLE, // /\/\/\/
	FORM_SILENCE,  // ______
	FORM_INC,      // _..--''
	FORM_DEC,      // ''--.._
};

enum {
	FILTER_INVERT,    // 1 -> 0
	FILTER_ATTACK,    // ____.'
	FILTER_DECAY,     // -----.
	FILTER_VOLUME,    // _..oo#
	FILTER_INC,       // ++++++
	FILTER_DEC,       // ------
	FILTER_INTERLACE, //  A + B
	FILTER_SHIFT,     // >> >>
	FILTER_ROTATE,    // >>> >>>
	FILTER_MOD,       // %
	FILTER_XOR,       // ^
	FILTER_SIGN,      // + -> -
	FILTER_SCALE,     // *=
};

short sample;
ao_device *device = NULL;
ao_sample_format format = {0};

void sample_filter(char *buf, int size, int filter, int value) {
	int i, j;
	int isize = size / 2;
	short *ibuf = (short*)buf;
	switch (filter) {
	case FILTER_ATTACK:
		value = isize / 100 * value;
		for (i = 0; i < isize; i++) {
			if (i < value) {
				int total_pending = value;
				int pending = value - i;
				float mul = pending / total_pending;
				ibuf[i] *= mul;
			}
		}
		break;
	case FILTER_DECAY:
		value = isize / 100 * value;
		for (i = 0; i < isize; i++) {
			if (i >= value) {
				float total_pending = isize - value;
				float pending = isize - i;
				float mul = pending / total_pending;
				ibuf[i] *= mul;
			}
		}
		break;
	case FILTER_XOR:
		for (i = 0; i + value< isize; i++) {
			// ibuf[i] ^= value; //ibuf[i + 1];
			ibuf[i] ^= ibuf[i + value];
		}
		break;
	case FILTER_INVERT:
		for (i = 0; i < isize; i++) {
			ibuf[i] = -ibuf[i];
		}
		break;
	case FILTER_DEC:
	case FILTER_INC:
		for (i = 0; i < isize; i++) {
			float pc = (float)i / (float)format.rate * 100;
			if (FILTER_INC == filter) {
				pc = 100 - pc;
			}
			pc /= value;
			pc += 1;
			if (!((int)i % (int)pc)) {
				ibuf[i] = 0xffff / 2;
			}
			else {
				//	sample = -max_sample;
			}
		}
		break;
	case FILTER_SHIFT:
		value = (isize / 100 * value);
		if (value > 0) {
			const int max = isize - value;
			for (i = 0; i < max; i++) {
				ibuf[i] = ibuf[i + value];
			}
			for (i = max; i < value; i++) {
				ibuf[i] = 0;
			}
		}
		else {
			/* TODO */
			const int max = isize - value;
			for (i = isize; i > value; i--) {
				ibuf[i] = ibuf[i - value];
			}
			for (i = 0; i < value; i++) {
				ibuf[i] = 0;
			}
		}
		break;
	case FILTER_SIGN:
		if (value > 0) {
			for (i = 0; i < isize; i++) {
				if (ibuf[i] > 0) {
					ibuf[i] = 0;
				}
			}
		}
		else {
			for (i = 0; i < isize; i++) {
				if (ibuf[i] < 0) {
					ibuf[i] = 0;
				}
			}
		}
		break;
	case FILTER_ROTATE:
		if (value > 0) {
			short *tmp = calloc(sizeof(short), value);
			for (i = 0; i< value; i++) {
				tmp[i] = ibuf[i];
			}
			const int max = isize - value;
			for (i = 0; i < max; i++) {
				ibuf[i] = ibuf[i + value];
			}
			for (i = max; i < value; i++) {
				ibuf[i] = tmp[i - max];
			}
			free(tmp);
		}
		else {
			/* TODO */
		}
		break;
	case FILTER_INTERLACE:
		if (value < 2) {
			value = 2;
		}
		for (i = 0; i < size / 2; i++) {
			if (!((i / value) % value)) {
				for (j = 0; j< value; j++) {
					ibuf[i] = 0;
				}
			}
		}
		break;
	case FILTER_SCALE:
		if (value < 100) {
			int j = value;
			for (i = 0; i + j < isize; i++) {
				ibuf[i] = ibuf[i + j];
				j++;
			}
			int base;
			for (base = i; i< isize; i++) {
				ibuf[i] = ibuf[i - base];
			}
		}
		else {
			// TODO
		}
		break;
	case FILTER_MOD:
		for (i = 0; i < isize; i++) {
			ibuf[i] = ibuf[i] / value * value;
		}
		break;
	case FILTER_VOLUME:
		for (i = 0; i < isize; i++) {
			ibuf[i] *= ((float)value / 100);
		}
		break;
	}
}

char *sample_new(float freq, int form, int *size) {
	int i;
	short sample; // float ?
	float max_sample = format.bits == 16 ? 0xffff / 2 : 0xff / 2;
	float volume = 1; // 0.8;
	float pc;
	// int buf_size = format.bits / 8 * format.channels * format.rate;
	int buf_size = 16 / 8 * format.channels * format.rate;
	char *buffer = calloc (buf_size, sizeof (char));
	if (!buffer) {
		return NULL;
	}
// eprintf ("bufsz %d\n", buf_size);
	if (size) {
		*size = buf_size; // 22050 // huh
		// eprintf ("sz = %d\n", *size);
	}
	short *word = (short*)(buffer);
	int words = buf_size / sizeof(short);
	for (i = 0; i < words; i++) {
		// sample = (char)(max_sample * sin(2 * M_PI * freq * ((float)i / format.rate * 2)));
//		sample = (char)(max_sample * sin(i * (freq / format.rate))); // 2 * M_PI * freq * ((float)i / format.rate * 2)));
		sample = (short) max_sample * sin (freq * (2 * M_PI) * i / format.rate);
// eprintf ("%d ", sample);

  // buffer[i] = sin(1000 * (2 * pi) * i / 44100);

		switch (form) {
		case FORM_SILENCE:
			sample = 0;
			break;
		case FORM_DEC:
		case FORM_INC:
			pc = (float)i / (float)format.rate * 100;
			if (form == FORM_INC) {
				pc = 100 - pc;
			}
			pc /= 11 ; // step -- should be parametrizable
			pc += 1;
			if (!((int)i % (int)pc)) {
				sample = max_sample;
			} else {
				sample = -max_sample;
			}
			break;
		case FORM_COS:
			sample = (int)(max_sample * cos (2 * M_PI * freq * ((float)i / format.rate * 2)));
			break;
		case FORM_SIN:
			// do nothing
			break;
		case FORM_SAW:
			{
				int rate = 14000 / freq;
				sample = ((i % rate) * (max_sample * 2) / rate) - max_sample;
				sample = -sample;
				// printf ("%f\n", (float)sample);
			}
			break;
		case FORM_TRIANGLE:
			{
				if (freq < 1) {
					freq = 1;
				}
				int rate = (14000 / freq) * 2;
				sample = ((i % rate) * (max_sample * 2) / rate) - max_sample;
			}
			break;
		case FORM_PULSE:
			sample = sample > 0 ? max_sample : -max_sample;
			break;
		case FORM_NOISE:
			sample = (rand() % (int)(max_sample * 2)) - max_sample;
			int s = (int)sample * (freq / 1000);
			if (s > 32700) {
				s = 32700;
			}
			if (s < -32700) {
				s = -32700;
			}
			sample = s;
			break;
		}
		sample *= volume;
// printf ("SAMP %d\n", sample);
		/* left channel */
		word[i] = sample;
		// buffer[2 * i] = sample & 0xf;
		// buffer[2 * i + 1] = (sample >> 4) & 0xff;
		// buffer[(2 * i) + 1] = ((unsigned short)sample >> 8) & 0xff;
		// i++;
	}
	// sample_filter (buffer, buf_size, FILTER_SIGN, 1);
	return buffer;
}

static bool au_init() {
	ao_initialize();

	int default_driver = ao_default_driver_id();

	format.bits = 16; // SID is 16bit, 8bit sounds too much like PDP
	format.channels = 1;
	format.rate = 22050;
	// format.rate = 11025;
	format.byte_format = AO_FMT_LITTLE;

	device = ao_open_live (default_driver, &format, NULL /* no options */);
	if (!device) {
		fprintf(stderr, "Error opening device.\n");
		return false;
	}
	return true;
}

static void au_help(RCore *core) {
	eprintf ("Usage: auw[type] [args]\n");
	eprintf (" fill current block with wave\n");
	eprintf (" args: frequence\n");
	eprintf (" types:\n"
		" (s)in    .''.''.\n"
		" (c)os    '..'..'\n"
		" (z)aw    /|/|/|/\n"
		" (p)ulse  |_|'|_|\n"
		" (n)oise  /:./|.:\n"
		" (t)ri..  /\\/\\/\\/\n"
		" (-)silen _______\n"
		" (i)nc    _..--''\n"
		" (d)ec    ''--.._\n"
	);
}

static bool au_write(RCore *core, const char *args) {
	int size = 0;
	char *sample = NULL;
	ut64 narg = r_num_math (core->num, args + 1);
	float arg = narg;
// eprintf ("ARG (%d)\n", (int)arg);
	if (arg == 0) {
		au_help (core);
		return true;
	}
	switch (*args) {
	case '?':
		au_help (core);
		break;
	case 's':
		sample = sample_new (arg, FORM_SIN, &size);
		break;
	case 't':
		sample = sample_new (arg, FORM_TRIANGLE, &size);
		break;
	case 'i':
		sample = sample_new (arg, FORM_INC, &size);
		break;
	case 'd':
		sample = sample_new (arg, FORM_DEC, &size);
		break;
	case 'c':
		sample = sample_new (arg, FORM_COS, &size);
		break;
	case 'p':
		sample = sample_new (arg, FORM_PULSE, &size);
		break;
	case 'n':
		sample = sample_new (arg, FORM_NOISE, &size);
		break;
	case 'z':
		sample = sample_new (arg, FORM_SAW, &size);
		break;
	case '-':
		sample = sample_new (arg, FORM_SILENCE, &size);
		break;
	}
	if (size > 0) {
		int i;
		for (i = 0; i < core->blocksize ; i+= size) {
			r_io_write_at (core->io, core->offset + i, (const ut8*)sample, size);
		}
	}
	r_core_block_read (core);
	free (sample);
	return true;
}

const char *asciiWaveSin[4] = {
	".''.''.'",
	"''.''.''",
	"'.''.''.",
	".''.''.'",
};

const char *asciiWaveCos[4] = {
	"..'..'..",
	".'..'..'",
	"'..'..'.",
	"..'..'..",
};

const char *asciiWaveTriangle[4] = {
	"/\\/\\/\\/\\",
	"\\/\\/\\/\\/",
	"/\\/\\/\\/\\",
	"\\/\\/\\/\\/",
};

const char *asciiWavePulse[4] = {
	"_|'|_|'|",
	"|'|_|'|_",
	"'|_|'|_|",
	"|_|'|_|'",
};

const char *asciiWaveNoise[4] = {
	"/:./|.:/",
	":./|.:/:",
	"./|.:/:.",
	"/|.:/:./"
};

const char *asciiWaveSilence[4] = {
	"________",
	"________",
	"________",
	"________",
};

const char *asciiWaveIncrement[4] = {
	"_..---''",
	"_..---''",
	"_..---''",
	"_..---''",
};

const char *asciiWaveDecrement[4] = {
	"''---.._",
	"''---.._",
	"''---.._",
	"''---.._",
};

const char *asciiWaveSaw[4] = {
	"/|/|/|/|",
	"|/|/|/|/",
	"/|/|/|/|",
	"|/|/|/|/",
};

static bool printWave(RCore *core) {
	short sample = 0;
	short *words = (short*)core->block;
	// TODO: shift with 'h' and 'l'
	int x , y, h;
	int i, nwords = core->blocksize / 2;
	int w = r_cons_get_size (&h) - 10;
#if 0
	h = 20;

	for (y = 0; y<h; y++) {
		for (x = 0; x<w; x++) {
			r_cons_printf ("#");
		}
		r_cons_printf ("\n");
	}
#endif
#if 1
	if (w < 1) w = 1;
	int min = 32768; //4200;
	int step = 1;
	for (i = 0; i < nwords; i+=step) {
		int x = i;
		int y = ((words[i]) + min) / 4096;
		if (y < 1) {
			y = 1;
		}
		if (x + 1 >= w) {
			break;
		}
		r_cons_gotoxy (x, y + 3);
		r_cons_printf ("#");
		// r_cons_printf ("%d %d - ", x, y);
	}
#endif
	return true;
}

static const char *asciin(int waveType) {
	int mod = waveType % 9;
	switch (mod) {
	case 0: return "sinus";
	case 1: return "cos..";
	case 2: return "tri..";
	case 3: return "pulse";
	case 4: return "noise";
	case 5: return "silen";
	case 6: return "incrm";
	case 7: return "decrm";
	case 8: return "saw..";
	}
	return NULL;
}

static const char *asciis(int i) {
	int mod = waveType % 9;
	i %= 4;
	switch (mod) {
	case 0: return asciiWaveSin[i];
	case 1: return asciiWaveCos[i];
	case 2: return asciiWaveTriangle[i];
	case 3: return asciiWavePulse[i];
	case 4: return asciiWaveNoise[i];
	case 5: return asciiWaveSilence[i];
	case 6: return asciiWaveIncrement[i];
	case 7: return asciiWaveDecrement[i];
	case 8: return asciiWaveSaw[i];
	}
	return NULL;
}

const char **aiis = {
	asciiWaveSin,
	asciiWaveCos,
	asciiWaveTriangle,
	asciiWavePulse,
	asciiWaveNoise,
	asciiWaveSilence,
	asciiWaveIncrement,
	asciiWaveDecrement,
	asciiWaveSaw,
};

typedef struct note_t {
	int type;
	int freq;
	int bsize; // TODO
	// TODO: add array of filters like volume, attack, decay, ...
} AUNote;

static AUNote notes[10];

static void au_note_play(RCore *core, int note) {
	waveType = notes[note].type;
	waveFreq = notes[note].freq;
	
	char waveTypeChar = "sctpn-idz"[waveType % 9];
	r_core_cmdf (core, "auw%c %d", waveTypeChar, waveFreq);
	r_core_cmd0 (core, "au.");
}

static void au_note_set(RCore *core, int note) {
	notes[note].type = waveType;
	notes[note].freq = waveFreq;
}

static bool au_visual_help(RCore *core) {
	r_cons_clear00 ();
	r_cons_printf ("Usage: auv - visual mode for audio processing\n\n");
	r_cons_printf (" jk -> change wave type (sin, saw, ..)\n");
	r_cons_printf (" hl -> seek around the buffer\n");
	r_cons_printf (" HL -> seek faster around the buffer\n");
	r_cons_printf (" n  -> assign current freq+type into [0-9] key\n");
	r_cons_printf (" 0-9-> play and write the recorded note\n");
	r_cons_printf (" +- -> increment/decrement the frequency\n");
	r_cons_printf (" pP -> rotate print modes\n");
	r_cons_printf (" .  -> play current block\n");
	r_cons_printf (" i  -> insert current note in current offset\n");
	r_cons_printf (" :  -> type r2 command\n");

	r_cons_flush(); // use less here
	r_cons_readchar();
}

static bool au_visual(RCore *core) {
	r_cons_flush ();
	r_cons_print_clear ();
	r_cons_clear00 ();

	ut64 now, base = r_sys_now () / 1000 / 500;
	int otdiff = 0;
	while (true) {
		now = r_sys_now () / 1000 / 500;
		int tdiff = now - base;
		const char *wave = asciis(waveType);
		const char *waveName = asciin(waveType);
		r_cons_clear00 ();
		//r_cons_gotoxy (0, 0);
		r_cons_printf ("[VisualWave] [0x%08"PFMT64x"] [%04x] %s %s freq %d\n",
			core->offset, tdiff, wave, waveName, waveFreq);
		switch (printMode % 2) {
		case 0:
			printWave (core);
			break;
		case 1:
		//	r_cons_gotoxy (0, 2);
			r_core_cmdf (core, "pxd2 ($r*16)-64");
			break;
		}
		if (tdiff + 1 > otdiff) {
		//	r_core_cmd (core, "au.", 0);
		}
		r_cons_flush();
	//	r_cons_visual_flush ();
		int ch = r_cons_readchar_timeout (500);
		char waveTypeChar = "sctpn-idz"[waveType % 9];
		switch (ch) {
		case 'p':
			printMode++;
			break;
		case 'P':
			printMode--;
			break;
		case '0':
			au_note_play (core, 0);
			break;
		case '1':
			au_note_play (core, 1);
			break;
		case '2':
			au_note_play (core, 2);
			break;
		case '3':
			au_note_play (core, 3);
			break;
		case '4':
			au_note_play (core, 4);
			break;
		case '5':
			au_note_play (core, 5);
			break;
		case '6':
			au_note_play (core, 6);
			break;
		case '7':
			au_note_play (core, 7);
			break;
		case '8':
			au_note_play (core, 8);
			break;
		case '9':
			au_note_play (core, 9);
			break;
		case 'n':
			r_cons_printf ("\nWhich note? (1 2 3 4 5 6 7 8 9 0) \n");
r_cons_flush();
			int ch = r_cons_readchar ();
			if (ch >= '0' && ch <= '9') {
				au_note_set (core, ch - '0');
			} else if (ch == 'q') {
				// foo
			} else {
				eprintf ("Invalid char\n");
				sleep(1);
			}
			break;
		case 'J':
			break;
		case '+':
			waveFreq += 20;
			r_core_cmdf (core, "auw%c %d", waveTypeChar, waveFreq);
			r_core_cmd0 (core, "au.");
			break;
		case '-':
			waveFreq -= 20;
			if (waveFreq < 20) {
				waveFreq = 10;
			}
			r_core_cmdf (core, "auw%c %d", waveTypeChar, waveFreq);
			r_core_cmd0 (core, "au.");
			break;
		case ':':
			r_core_visual_prompt_input (core);
			break;
		case 'H':
			r_core_seek_delta (core, -32);
			break;
		case 'h':
			r_core_seek_delta (core, -2);
			break;
		case 'l':
			r_core_seek_delta (core, 2);
			break;
		case 'L':
			r_core_seek_delta (core, 32);
			break;
		case 'j':
			waveType++;
			break;
		case '?':
			au_visual_help (core);
			break;
		case 'k':
			waveType--;
			break;
		case 'i':
			r_core_cmdf (core, "auws %d", waveFreq);
			break;
		case '.':
			// TODO : run in a thread?
			r_core_cmd0 (core, "au.");
			break;
		case 'q':
			return false;
			break;
		}
	}
	
	return false;
}

static bool au_play(RCore *core) {
	ao_play (device, (char *)core->block, core->blocksize);
	// eprintf ("Played %d bytes\n", core->blocksize);
	return false;
}

static int _cmd_au (RCore *core, const char *args) {
	switch (*args) {
	case 'i':
		// setup arguments here
		au_init ();
		break;
	case 'w':
		// write pattern here
		au_write (core, args + 1);
		break;
	case 'p':
		printWave (core);
		break;
	case '.':
		// write pattern here
		au_play (core);
		break;
	case 'v':
		au_visual (core);
		break;
	default:
	case '?':
		eprintf ("Usage: au[i] [args]\n");
		eprintf (" aui - init audio\n");
		eprintf (" au. - play current block\n");
		eprintf (" aup - print wave\n");
		eprintf (" auw - write wave (see auw?)\n");
		eprintf (" auv - visual wave mode\n");
		break;
	}
	return false;
}

static int r_cmd_au_call(void *user, const char *input) {
	RCore *core = (RCore *) user;
	if (!strncmp (input, "au", 2)) {
		_cmd_au (core, input + 2);
		return true;
	}
	return false;
}

RCorePlugin r_core_plugin_au = {
	.name = "audio",
	.desc = "play mustic with radare2",
	.license = "MIT",
	.call = r_cmd_au_call,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_CORE,
	.data = &r_core_plugin_au,
	.version = R2_VERSION
};
#endif

