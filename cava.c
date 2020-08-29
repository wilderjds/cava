#include <locale.h>

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#else
#include <stdlib.h>
#endif

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

#include <ctype.h>
#include <dirent.h>
#include <fftw3.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"
#include "util.h"

#ifdef NCURSES
#include "output/terminal_bcircle.h"
#include "output/terminal_ncurses.h"
#include <curses.h>
#endif

#include "output/raw.h"
#include "output/terminal_noncurses.h"

#include "input/alsa.h"
#include "input/common.h"
#include "input/fifo.h"
#include "input/portaudio.h"
#include "input/pulse.h"
#include "input/shmem.h"
#include "input/sndio.h"

#include "config.h"

#ifdef __GNUC__
// curses.h or other sources may already define
#undef GCC_UNUSED
#define GCC_UNUSED __attribute__((unused))
#else
#define GCC_UNUSED /* nothing */
#endif

#define LEFT_CHANNEL 1
#define RIGHT_CHANNEL 2

// struct termios oldtio, newtio;
// int M = 8 * 1024;

// used by sig handler
// needs to know output mode in orer to clean up terminal
int output_mode;
// whether we should reload the config or not
int should_reload = 0;
// whether we should only reload colors or not
int reload_colors = 0;

// these variables are used only in main, but making them global
// will allow us to not free them on exit without ASan complaining
struct config_params p;

fftw_complex *out_bass_l, *out_bass_r;
fftw_plan p_bass_l, p_bass_r;
fftw_complex *out_mid_l, *out_mid_r;
fftw_plan p_mid_l, p_mid_r;
fftw_complex *out_treble_l, *out_treble_r;
fftw_plan p_treble_l, p_treble_r;

// general: cleanup
void cleanup(void) {
    if (output_mode == OUTPUT_NCURSES) {
#ifdef NCURSES
        cleanup_terminal_ncurses();
#else
        ;
#endif
    } else if (output_mode == OUTPUT_NONCURSES) {
        cleanup_terminal_noncurses();
    }
}

// general: handle signals
void sig_handler(int sig_no) {
    if (sig_no == SIGUSR1) {
        should_reload = 1;
        return;
    }

    if (sig_no == SIGUSR2) {
        reload_colors = 1;
        return;
    }

    cleanup();
    if (sig_no == SIGINT) {
        printf("CTRL-C pressed -- goodbye\n");
    }
    signal(sig_no, SIG_DFL);
    raise(sig_no);
}

#ifdef ALSA
static bool is_loop_device_for_sure(const char *text) {
    const char *const LOOPBACK_DEVICE_PREFIX = "hw:Loopback,";
    return strncmp(text, LOOPBACK_DEVICE_PREFIX, strlen(LOOPBACK_DEVICE_PREFIX)) == 0;
}

static bool directory_exists(const char *path) {
    DIR *const dir = opendir(path);
    if (dir == NULL)
        return false;

    closedir(dir);
    return true;
}

#endif

int *separate_freq_bands(int FFTbassbufferSize, fftw_complex out_bass[FFTbassbufferSize / 2 + 1],
                         int FFTmidbufferSize, fftw_complex out_mid[FFTmidbufferSize / 2 + 1],
                         int FFTtreblebufferSize,
                         fftw_complex out_treble[FFTtreblebufferSize / 2 + 1], int bass_cut_off_bar,
                         int treble_cut_off_bar, int number_of_bars,
                         int FFTbuffer_lower_cut_off[256], int FFTbuffer_upper_cut_off[256],
                         double eq[256], int channel, double sens, double ignore) {
    int n, i;
    double peak[257];
    static int bars_left[256];
    static int bars_right[256];
    double y[FFTbassbufferSize / 2 + 1];
    double temp;

    // process: separate frequency bands
    for (n = 0; n < number_of_bars; n++) {

        peak[n] = 0;
        i = 0;

        // process: get peaks
        for (i = FFTbuffer_lower_cut_off[n]; i <= FFTbuffer_upper_cut_off[n]; i++) {
            if (n <= bass_cut_off_bar) {
                y[i] = hypot(out_bass[i][0], out_bass[i][1]);
            } else if (n > bass_cut_off_bar && n <= treble_cut_off_bar) {
                y[i] = hypot(out_mid[i][0], out_mid[i][1]);
            } else if (n > treble_cut_off_bar) {
                y[i] = hypot(out_treble[i][0], out_treble[i][1]);
            }

            peak[n] += y[i]; // adding upp band
        }

        peak[n] = peak[n] /
                  (FFTbuffer_upper_cut_off[n] - FFTbuffer_lower_cut_off[n] + 1); // getting average
        temp = peak[n] * sens * eq[n]; // multiplying with k and sens
        // printf("%d peak o: %f * sens: %f * k: %f = f: %f\n", o, peak[o], sens, eq[o], temp);
        if (temp <= ignore)
            temp = 0;
        if (channel == LEFT_CHANNEL)
            bars_left[n] = temp;
        else
            bars_right[n] = temp;
    }

    if (channel == LEFT_CHANNEL)
        return bars_left;
    else
        return bars_right;
}

int *monstercat_filter(int *bars, int number_of_bars, int waves, double monstercat) {

    int z;

    // process [smoothing]: monstercat-style "average"

    int m_y, de;
    if (waves > 0) {
        for (z = 0; z < number_of_bars; z++) { // waves
            bars[z] = bars[z] / 1.25;
            // if (bars[z] < 1) bars[z] = 1;
            for (m_y = z - 1; m_y >= 0; m_y--) {
                de = z - m_y;
                bars[m_y] = max(bars[z] - pow(de, 2), bars[m_y]);
            }
            for (m_y = z + 1; m_y < number_of_bars; m_y++) {
                de = m_y - z;
                bars[m_y] = max(bars[z] - pow(de, 2), bars[m_y]);
            }
        }
    } else if (monstercat > 0) {
        for (z = 0; z < number_of_bars; z++) {
            // if (bars[z] < 1)bars[z] = 1;
            for (m_y = z - 1; m_y >= 0; m_y--) {
                de = z - m_y;
                bars[m_y] = max(bars[z] / pow(monstercat, de), bars[m_y]);
            }
            for (m_y = z + 1; m_y < number_of_bars; m_y++) {
                de = m_y - z;
                bars[m_y] = max(bars[z] / pow(monstercat, de), bars[m_y]);
            }
        }
    }

    return bars;
}

// general: entry point
int main(int argc, char **argv) {

    // general: define variables
    pthread_t p_thread;
    int thr_id GCC_UNUSED;
    float cut_off_frequency[256];
    float relative_cut_off[256];
    int bars[256], FFTbuffer_lower_cut_off[256], FFTbuffer_upper_cut_off[256];
    int *bars_left, *bars_right, *bars_mono;
    int bars_mem[256];
    int bars_last[256];
    int previous_frame[256];
    int sleep = 0;
    int n, height, lines, width, c, rest, inAtty, fp, fptest, rc;
    bool silence;
    // int cont = 1;
    int fall[256];
    // float temp;
    float bars_peak[256];
    double eq[256];
    float g;
    struct timespec req = {.tv_sec = 0, .tv_nsec = 0};
    struct timespec sleep_mode_timer = {.tv_sec = 0, .tv_nsec = 0};
    char configPath[PATH_MAX];
    char *usage = "\n\
Usage : " PACKAGE " [options]\n\
Visualize audio input in terminal. \n\
\n\
Options:\n\
	-p          path to config file\n\
	-v          print version\n\
\n\
Keys:\n\
        Up        Increase sensitivity\n\
        Down      Decrease sensitivity\n\
        Left      Decrease number of bars\n\
        Right     Increase number of bars\n\
        r         Reload config\n\
        c         Reload colors only\n\
        f         Cycle foreground color\n\
        b         Cycle background color\n\
        q         Quit\n\
\n\
as of 0.4.0 all options are specified in config file, see in '/home/username/.config/cava/' \n";

    char ch = '\0';
    int number_of_bars = 25;
    int sourceIsAuto = 1;
    double userEQ_keys_to_bars_ratio;

    struct audio_data audio;
    memset(&audio, 0, sizeof(audio));

#ifndef NDEBUG
    int maxvalue = 0;
    int minvalue = 0;
#endif
    // general: console title
    printf("%c]0;%s%c", '\033', PACKAGE, '\007');

    configPath[0] = '\0';

    // general: handle Ctrl+C
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = &sig_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGUSR2, &action, NULL);

    // general: handle command-line arguments
    while ((c = getopt(argc, argv, "p:vh")) != -1) {
        switch (c) {
        case 'p': // argument: fifo path
            snprintf(configPath, sizeof(configPath), "%s", optarg);
            break;
        case 'h': // argument: print usage
            printf("%s", usage);
            return 1;
        case '?': // argument: print usage
            printf("%s", usage);
            return 1;
        case 'v': // argument: print version
            printf(PACKAGE " " VERSION "\n");
            return 0;
        default: // argument: no arguments; exit
            abort();
        }

        n = 0;
    }

    // general: main loop
    while (1) {

        debug("loading config\n");
        // config: load
        struct error_s error;
        error.length = 0;
        if (!load_config(configPath, &p, 0, &error)) {
            fprintf(stderr, "Error loading config. %s", error.message);
            exit(EXIT_FAILURE);
        }

        output_mode = p.om;

        if (output_mode != OUTPUT_RAW) {
            // Check if we're running in a tty
            inAtty = 0;
            if (strncmp(ttyname(0), "/dev/tty", 8) == 0 || strcmp(ttyname(0), "/dev/console") == 0)
                inAtty = 1;

            // in macos vitual terminals are called ttys(xyz) and there are no ttys
            if (strncmp(ttyname(0), "/dev/ttys", 9) == 0)
                inAtty = 0;
            if (inAtty) {
                system("setfont cava.psf  >/dev/null 2>&1");
                system("setterm -blank 0");
            }

            // We use unicode block characters to draw the bars and
            // the locale var LANG must be set to use unicode chars.
            // For some reason this var can't be retrieved with
            // setlocale(LANG, NULL), so we get it with getenv.
            // Also we can't set it with setlocale(LANG "") so we
            // must set LC_ALL instead.
            // Attempting to set to en_US if not set, if that lang
            // is not installed and LANG is not set there will be
            // no output, for mor info see #109 #344
            if (!getenv("LANG"))
                setlocale(LC_ALL, "en_US.utf8");
            else
                setlocale(LC_ALL, "");
        }

        // input: init
        int bass_cut_off = 150;
        int treble_cut_off = 1500;

        audio.source = malloc(1 + strlen(p.audio_source));
        strcpy(audio.source, p.audio_source);

        audio.format = -1;
        audio.rate = 0;
        audio.FFTbassbufferSize = 4096;
        audio.FFTmidbufferSize = 1024;
        audio.FFTtreblebufferSize = 512;
        audio.terminate = 0;
        if (p.stereo)
            audio.channels = 2;
        if (!p.stereo)
            audio.channels = 1;
        audio.average = false;
        audio.left = false;
        audio.right = false;
        if (strcmp(p.mono_option, "average") == 0)
            audio.average = true;
        if (strcmp(p.mono_option, "left") == 0)
            audio.left = true;
        if (strcmp(p.mono_option, "right") == 0)
            audio.right = true;
        audio.bass_index = 0;
        audio.mid_index = 0;
        audio.treble_index = 0;

        // BASS
        // audio.FFTbassbufferSize =  audio.rate / 20; // audio.FFTbassbufferSize;

        audio.in_bass_r = fftw_alloc_real(2 * (audio.FFTbassbufferSize / 2 + 1));
        audio.in_bass_l = fftw_alloc_real(2 * (audio.FFTbassbufferSize / 2 + 1));
        memset(audio.in_bass_r, 0, 2 * (audio.FFTbassbufferSize / 2 + 1) * sizeof(double));
        memset(audio.in_bass_l, 0, 2 * (audio.FFTbassbufferSize / 2 + 1) * sizeof(double));

        out_bass_l = fftw_alloc_complex(2 * (audio.FFTbassbufferSize / 2 + 1));
        out_bass_r = fftw_alloc_complex(2 * (audio.FFTbassbufferSize / 2 + 1));
        memset(out_bass_l, 0, 2 * (audio.FFTbassbufferSize / 2 + 1) * sizeof(fftw_complex));
        memset(out_bass_r, 0, 2 * (audio.FFTbassbufferSize / 2 + 1) * sizeof(fftw_complex));

        p_bass_l = fftw_plan_dft_r2c_1d(audio.FFTbassbufferSize, audio.in_bass_l, out_bass_l,
                                        FFTW_MEASURE);
        p_bass_r = fftw_plan_dft_r2c_1d(audio.FFTbassbufferSize, audio.in_bass_r, out_bass_r,
                                        FFTW_MEASURE);

        // MID
        // audio.FFTmidbufferSize =  audio.rate / bass_cut_off; // audio.FFTbassbufferSize;
        audio.in_mid_r = fftw_alloc_real(2 * (audio.FFTmidbufferSize / 2 + 1));
        audio.in_mid_l = fftw_alloc_real(2 * (audio.FFTmidbufferSize / 2 + 1));
        memset(audio.in_mid_r, 0, 2 * (audio.FFTmidbufferSize / 2 + 1) * sizeof(double));
        memset(audio.in_mid_l, 0, 2 * (audio.FFTmidbufferSize / 2 + 1) * sizeof(double));

        out_mid_l = fftw_alloc_complex(2 * (audio.FFTmidbufferSize / 2 + 1));
        out_mid_r = fftw_alloc_complex(2 * (audio.FFTmidbufferSize / 2 + 1));
        memset(out_mid_l, 0, 2 * (audio.FFTmidbufferSize / 2 + 1) * sizeof(fftw_complex));
        memset(out_mid_r, 0, 2 * (audio.FFTmidbufferSize / 2 + 1) * sizeof(fftw_complex));

        p_mid_l =
            fftw_plan_dft_r2c_1d(audio.FFTmidbufferSize, audio.in_mid_l, out_mid_l, FFTW_MEASURE);
        p_mid_r =
            fftw_plan_dft_r2c_1d(audio.FFTmidbufferSize, audio.in_mid_r, out_mid_r, FFTW_MEASURE);

        // TRIEBLE
        // audio.FFTtreblebufferSize =  audio.rate / treble_cut_off; // audio.FFTbassbufferSize;
        audio.in_treble_r = fftw_alloc_real(2 * (audio.FFTtreblebufferSize / 2 + 1));
        audio.in_treble_l = fftw_alloc_real(2 * (audio.FFTtreblebufferSize / 2 + 1));
        memset(audio.in_treble_r, 0, 2 * (audio.FFTtreblebufferSize / 2 + 1) * sizeof(double));
        memset(audio.in_treble_l, 0, 2 * (audio.FFTtreblebufferSize / 2 + 1) * sizeof(double));

        out_treble_l = fftw_alloc_complex(2 * (audio.FFTtreblebufferSize / 2 + 1));
        out_treble_r = fftw_alloc_complex(2 * (audio.FFTtreblebufferSize / 2 + 1));
        memset(out_treble_l, 0, 2 * (audio.FFTtreblebufferSize / 2 + 1) * sizeof(fftw_complex));
        memset(out_treble_r, 0, 2 * (audio.FFTtreblebufferSize / 2 + 1) * sizeof(fftw_complex));

        p_treble_l = fftw_plan_dft_r2c_1d(audio.FFTtreblebufferSize, audio.in_treble_l,
                                          out_treble_l, FFTW_MEASURE);
        p_treble_r = fftw_plan_dft_r2c_1d(audio.FFTtreblebufferSize, audio.in_treble_r,
                                          out_treble_r, FFTW_MEASURE);

        debug("got buffer size: %d, %d, %d", audio.FFTbassbufferSize, audio.FFTmidbufferSize,
              audio.FFTtreblebufferSize);

        reset_output_buffers(&audio);

        debug("starting audio thread\n");
        switch (p.im) {
#ifdef ALSA
        case INPUT_ALSA:
            // input_alsa: wait for the input to be ready
            if (is_loop_device_for_sure(audio.source)) {
                if (directory_exists("/sys/")) {
                    if (!directory_exists("/sys/module/snd_aloop/")) {
                        cleanup();
                        fprintf(stderr,
                                "Linux kernel module \"snd_aloop\" does not seem to  be loaded.\n"
                                "Maybe run \"sudo modprobe snd_aloop\".\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }

            thr_id = pthread_create(&p_thread, NULL, input_alsa,
                                    (void *)&audio); // starting alsamusic listener

            n = 0;

            while (audio.format == -1 || audio.rate == 0) {
                req.tv_sec = 0;
                req.tv_nsec = 1000000;
                nanosleep(&req, NULL);
                n++;
                if (n > 2000) {
                    cleanup();
                    fprintf(stderr, "could not get rate and/or format, problems with audio thread? "
                                    "quiting...\n");
                    exit(EXIT_FAILURE);
                }
            }
            debug("got format: %d and rate %d\n", audio.format, audio.rate);
            break;
#endif
        case INPUT_FIFO:
            // starting fifomusic listener
            thr_id = pthread_create(&p_thread, NULL, input_fifo, (void *)&audio);
            audio.rate = p.fifoSample;
            audio.format = p.fifoSampleBits;
            break;
#ifdef PULSE
        case INPUT_PULSE:
            if (strcmp(audio.source, "auto") == 0) {
                getPulseDefaultSink((void *)&audio);
                sourceIsAuto = 1;
            } else
                sourceIsAuto = 0;
            // starting pulsemusic listener
            thr_id = pthread_create(&p_thread, NULL, input_pulse, (void *)&audio);
            audio.rate = 44100;
            break;
#endif
#ifdef SNDIO
        case INPUT_SNDIO:
            thr_id = pthread_create(&p_thread, NULL, input_sndio, (void *)&audio);
            audio.rate = 44100;
            break;
#endif
        case INPUT_SHMEM:
            thr_id = pthread_create(&p_thread, NULL, input_shmem, (void *)&audio);
            // audio.rate = 44100;
            break;
#ifdef PORTAUDIO
        case INPUT_PORTAUDIO:
            thr_id = pthread_create(&p_thread, NULL, input_portaudio, (void *)&audio);
            audio.rate = 44100;
            break;
#endif
        default:
            exit(EXIT_FAILURE); // Can't happen.
        }

        if (p.upper_cut_off > audio.rate / 2) {
            cleanup();
            fprintf(stderr, "higher cuttoff frequency can't be higher then sample rate / 2");
            exit(EXIT_FAILURE);
        }

        bool reloadConf = false;

        while (!reloadConf) { // jumbing back to this loop means that you resized the screen
            for (n = 0; n < 256; n++) {
                bars_last[n] = 0;
                previous_frame[n] = 0;
                fall[n] = 0;
                bars_peak[n] = 0;
                bars_mem[n] = 0;
                bars[n] = 0;
            }

            switch (output_mode) {
#ifdef NCURSES
            // output: start ncurses mode
            case OUTPUT_NCURSES:
                init_terminal_ncurses(p.color, p.bcolor, p.col, p.bgcol, p.gradient,
                                      p.gradient_count, p.gradient_colors, p.gradient_discrete, &width, &lines);
                // we have 8 times as much height due to using 1/8 block characters
                height = lines * 8;
                break;
#endif
            case OUTPUT_NONCURSES:
                get_terminal_dim_noncurses(&width, &lines);
                init_terminal_noncurses(inAtty, p.col, p.bgcol, width, lines, p.bar_width);
                height = (lines - 1) * 8;
                break;

            case OUTPUT_RAW:
                if (strcmp(p.raw_target, "/dev/stdout") != 0) {
                    // checking if file exists
                    if (access(p.raw_target, F_OK) != -1) {
                        // testopening in case it's a fifo
                        fptest = open(p.raw_target, O_RDONLY | O_NONBLOCK, 0644);

                        if (fptest == -1) {
                            printf("could not open file %s for writing\n", p.raw_target);
                            exit(1);
                        }
                    } else {
                        printf("creating fifo %s\n", p.raw_target);
                        if (mkfifo(p.raw_target, 0664) == -1) {
                            printf("could not create fifo %s\n", p.raw_target);
                            exit(1);
                        }
                        // fifo needs to be open for reading in order to write to it
                        fptest = open(p.raw_target, O_RDONLY | O_NONBLOCK, 0644);
                    }
                }

                fp = open(p.raw_target, O_WRONLY | O_NONBLOCK | O_CREAT, 0644);
                if (fp == -1) {
                    printf("could not open file %s for writing\n", p.raw_target);
                    exit(1);
                }
                printf("open file %s for writing raw output\n", p.raw_target);

                // width must be hardcoded for raw output.
                width = 256;

                if (strcmp(p.data_format, "binary") == 0) {
                    height = pow(2, p.bit_format) - 1;
                } else {
                    height = p.ascii_range;
                }
                break;

            default:
                exit(EXIT_FAILURE); // Can't happen.
            }

            // handle for user setting too many bars
            if (p.fixedbars) {
                p.autobars = 0;
                if (p.fixedbars * p.bar_width + p.fixedbars * p.bar_spacing - p.bar_spacing > width)
                    p.autobars = 1;
            }

            // getting orignial numbers of barss incase of resize
            if (p.autobars == 1) {
                number_of_bars = (width + p.bar_spacing) / (p.bar_width + p.bar_spacing);
                // if (p.bar_spacing != 0) number_of_bars = (width - number_of_bars * p.bar_spacing
                // + p.bar_spacing) / bar_width;
            } else
                number_of_bars = p.fixedbars;

            if (number_of_bars < 1)
                number_of_bars = 1; // must have at least 1 bars
            if (number_of_bars > 256)
                number_of_bars = 256; // cant have more than 256 bars

            if (p.stereo) { // stereo must have even numbers of bars
                if (number_of_bars % 2 != 0)
                    number_of_bars--;
            }

            // checks if there is stil extra room, will use this to center
            rest = (width - number_of_bars * p.bar_width - number_of_bars * p.bar_spacing +
                    p.bar_spacing) /
                   2;
            if (rest < 0)
                rest = 0;

            // process [smoothing]: calculate gravity
            g = p.gravity * ((float)height / 2160) * pow((60 / (float)p.framerate), 2.5);

            // calculate integral value, must be reduced with height
            double integral = p.integral;
            if (height > 320)
                integral = p.integral * 1 / sqrt((log10((float)height / 10)));

#ifndef NDEBUG
            debug("height: %d width: %d bars:%d bar width: %d rest: %d\n", height, width,
                  number_of_bars, p.bar_width, rest);
#endif

            if (p.stereo)
                number_of_bars =
                    number_of_bars / 2; // in stereo onle half number of number_of_bars per channel

            if (p.userEQ_enabled && (number_of_bars > 0)) {
                userEQ_keys_to_bars_ratio =
                    (double)(((double)p.userEQ_keys) / ((double)number_of_bars));
            }

            // calculate frequency constant (used to distribute bars across the frequency band)
            double frequency_constant = log10((float)p.lower_cut_off / (float)p.upper_cut_off) /
                                        (1 / ((float)number_of_bars + 1) - 1);

            // process: calculate cutoff frequencies and eq
            int bass_cut_off_bar = -1;
            int treble_cut_off_bar = -1;
            bool first_bar = false;
            int first_treble_bar = 0;

            for (n = 0; n < number_of_bars + 1; n++) {
                double bar_distribution_coefficient = frequency_constant * (-1);
                bar_distribution_coefficient +=
                    ((float)n + 1) / ((float)number_of_bars + 1) * frequency_constant;
                cut_off_frequency[n] = p.upper_cut_off * pow(10, bar_distribution_coefficient);
                relative_cut_off[n] = cut_off_frequency[n] / (audio.rate / 2);
                // remember nyquist!, pr my calculations this should be rate/2
                // and  nyquist freq in M/2 but testing shows it is not...
                // or maybe the nq freq is in M/4

                eq[n] = pow(cut_off_frequency[n], 1);
                eq[n] *= (float)height / pow(2, 28);
                if (p.userEQ_enabled)
                    eq[n] *= p.userEQ[(int)floor(((double)n) * userEQ_keys_to_bars_ratio)];

                eq[n] /= log2(audio.FFTbassbufferSize);

                if (cut_off_frequency[n] < bass_cut_off) {
                    // BASS
                    FFTbuffer_lower_cut_off[n] =
                        relative_cut_off[n] * (audio.FFTbassbufferSize / 2) + 1;
                    bass_cut_off_bar++;
                    treble_cut_off_bar++;

                    eq[n] *= log2(audio.FFTbassbufferSize);
                } else if (cut_off_frequency[n] > bass_cut_off &&
                           cut_off_frequency[n] < treble_cut_off) {
                    // MID
                    FFTbuffer_lower_cut_off[n] =
                        relative_cut_off[n] * (audio.FFTmidbufferSize / 2) + 1;
                    treble_cut_off_bar++;
                    if ((treble_cut_off_bar - bass_cut_off_bar) == 1) {
                        first_bar = true;
                        FFTbuffer_upper_cut_off[n - 1] =
                            relative_cut_off[n] * (audio.FFTbassbufferSize / 2);
                        if (FFTbuffer_upper_cut_off[n - 1] < FFTbuffer_lower_cut_off[n - 1])
                            FFTbuffer_upper_cut_off[n - 1] = FFTbuffer_lower_cut_off[n - 1];
                    } else {
                        first_bar = false;
                    }

                    eq[n] *= log2(audio.FFTmidbufferSize);
                } else {
                    // TREBLE
                    FFTbuffer_lower_cut_off[n] =
                        relative_cut_off[n] * (audio.FFTtreblebufferSize / 2) + 1;
                    first_treble_bar++;
                    if (first_treble_bar == 1) {
                        first_bar = true;
                        FFTbuffer_upper_cut_off[n - 1] =
                            relative_cut_off[n] * (audio.FFTmidbufferSize / 2);
                        if (FFTbuffer_upper_cut_off[n - 1] < FFTbuffer_lower_cut_off[n - 1])
                            FFTbuffer_upper_cut_off[n - 1] = FFTbuffer_lower_cut_off[n - 1];
                    } else {
                        first_bar = false;
                    }

                    eq[n] *= log2(audio.FFTtreblebufferSize);
                }

                if (n != 0 && !first_bar) {
                    FFTbuffer_upper_cut_off[n - 1] = FFTbuffer_lower_cut_off[n] - 1;

                    // pushing the spectrum up if the exponential function gets "clumped" in the
                    // bass
                    if (FFTbuffer_lower_cut_off[n] <= FFTbuffer_lower_cut_off[n - 1])
                        FFTbuffer_lower_cut_off[n] = FFTbuffer_lower_cut_off[n - 1] + 1;
                    FFTbuffer_upper_cut_off[n - 1] = FFTbuffer_lower_cut_off[n] - 1;
                }

#ifndef NDEBUG
                initscr();
                curs_set(0);
                timeout(0);
                if (n != 0) {
                    mvprintw(n, 0, "%d: %f -> %f (%d -> %d) bass: %d, treble:%d \n", n,
                             cut_off_frequency[n - 1], cut_off_frequency[n],
                             FFTbuffer_lower_cut_off[n - 1], FFTbuffer_upper_cut_off[n - 1],
                             bass_cut_off_bar, treble_cut_off_bar);
                }
#endif
            }

            if (p.stereo)
                number_of_bars = number_of_bars * 2;

            bool resizeTerminal = false;
            fcntl(0, F_SETFL, O_NONBLOCK);

            if (p.framerate <= 1) {
                req.tv_sec = 1 / (float)p.framerate;
            } else {
                req.tv_sec = 0;
                req.tv_nsec = (1 / (float)p.framerate) * 1e9;
            }

            while (!resizeTerminal) {

// general: keyboard controls
#ifdef NCURSES
                if (output_mode == OUTPUT_NCURSES)
                    ch = getch();
#endif
                if (output_mode == OUTPUT_NONCURSES)
                    ch = fgetc(stdin);

                switch (ch) {
                case 65: // key up
                    p.sens = p.sens * 1.05;
                    break;
                case 66: // key down
                    p.sens = p.sens * 0.95;
                    break;
                case 68: // key right
                    p.bar_width++;
                    resizeTerminal = true;
                    break;
                case 67: // key left
                    if (p.bar_width > 1)
                        p.bar_width--;
                    resizeTerminal = true;
                    break;
                case 'r': // reload config
                    should_reload = 1;
                    break;
                case 'c': // reload colors
                    reload_colors = 1;
                    break;
                case 'f': // change forground color
                    if (p.col < 7)
                        p.col++;
                    else
                        p.col = 0;
                    resizeTerminal = true;
                    break;
                case 'b': // change backround color
                    if (p.bgcol < 7)
                        p.bgcol++;
                    else
                        p.bgcol = 0;
                    resizeTerminal = true;
                    break;

                case 'q':
                    if (sourceIsAuto)
                        free(audio.source);
                    cleanup();
                    return EXIT_SUCCESS;
                }

                if (should_reload) {

                    reloadConf = true;
                    resizeTerminal = true;
                    should_reload = 0;
                }

                if (reload_colors) {
                    struct error_s error;
                    error.length = 0;
                    if (!load_config(configPath, (void *)&p, 1, &error)) {
                        cleanup();
                        fprintf(stderr, "Error loading config. %s", error.message);
                        exit(EXIT_FAILURE);
                    }
                    resizeTerminal = true;
                    reload_colors = 0;
                }

                // if (cont == 0) break;

#ifndef NDEBUG
                // clear();
                refresh();
#endif

                // process: check if input is present
                silence = true;

                for (n = 0; n < audio.FFTbassbufferSize; n++) {
                    if (audio.in_bass_l[n] || audio.in_bass_r[n]) {
                        silence = false;
                        break;
                    }
                }

                if (silence)
                    sleep++;
                else
                    sleep = 0;

                // process: if input was present for the last 5 seconds apply FFT to it
                if (sleep < p.framerate * 5) {

                    // process: execute FFT and sort frequency bands
                    if (p.stereo) {
                        fftw_execute(p_bass_l);
                        fftw_execute(p_bass_r);
                        fftw_execute(p_mid_l);
                        fftw_execute(p_mid_r);
                        fftw_execute(p_treble_l);
                        fftw_execute(p_treble_r);

                        bars_left = separate_freq_bands(
                            audio.FFTbassbufferSize, out_bass_l, audio.FFTmidbufferSize, out_mid_l,
                            audio.FFTtreblebufferSize, out_treble_l, bass_cut_off_bar,
                            treble_cut_off_bar, number_of_bars / 2, FFTbuffer_lower_cut_off,
                            FFTbuffer_upper_cut_off, eq, LEFT_CHANNEL, p.sens, p.ignore);

                        bars_right = separate_freq_bands(
                            audio.FFTbassbufferSize, out_bass_r, audio.FFTmidbufferSize, out_mid_r,
                            audio.FFTtreblebufferSize, out_treble_r, bass_cut_off_bar,
                            treble_cut_off_bar, number_of_bars / 2, FFTbuffer_lower_cut_off,
                            FFTbuffer_upper_cut_off, eq, RIGHT_CHANNEL, p.sens, p.ignore);

                    } else {
                        fftw_execute(p_bass_l);
                        fftw_execute(p_mid_l);
                        fftw_execute(p_treble_l);
                        bars_mono = separate_freq_bands(
                            audio.FFTbassbufferSize, out_bass_l, audio.FFTmidbufferSize, out_mid_l,
                            audio.FFTtreblebufferSize, out_treble_l, bass_cut_off_bar,
                            treble_cut_off_bar, number_of_bars, FFTbuffer_lower_cut_off,
                            FFTbuffer_upper_cut_off, eq, LEFT_CHANNEL, p.sens, p.ignore);
                    }

                } else { //**if in sleep mode wait and continue**//
#ifndef NDEBUG
                    printw("no sound detected for 5 sec, going to sleep mode\n");
#endif
                    // wait 0.1 sec, then check sound again.
                    sleep_mode_timer.tv_sec = 0;
                    sleep_mode_timer.tv_nsec = 100000000;
                    nanosleep(&sleep_mode_timer, NULL);
                    continue;
                }

                // process [filter]

                if (p.monstercat) {
                    if (p.stereo) {
                        bars_left =
                            monstercat_filter(bars_left, number_of_bars / 2, p.waves, p.monstercat);
                        bars_right = monstercat_filter(bars_right, number_of_bars / 2, p.waves,
                                                       p.monstercat);
                    } else {
                        bars_mono =
                            monstercat_filter(bars_mono, number_of_bars, p.waves, p.monstercat);
                    }
                }

                // processing signal

                bool senselow = true;

                for (n = 0; n < number_of_bars; n++) {
                    // mirroring stereo channels
                    if (p.stereo) {
                        if (n < number_of_bars / 2) {
                            bars[n] = bars_left[number_of_bars / 2 - n - 1];
                        } else {
                            bars[n] = bars_right[n - number_of_bars / 2];
                        }

                    } else {
                        bars[n] = bars_mono[n];
                    }

                    // process [smoothing]: falloff
                    if (g > 0) {
                        if (bars[n] < bars_last[n]) {
                            bars[n] = bars_peak[n] - (g * fall[n] * fall[n]);
                            if (bars[n] < 0)
                                bars[n] = 0;
                            fall[n]++;
                        } else {
                            bars_peak[n] = bars[n];
                            fall[n] = 0;
                        }

                        bars_last[n] = bars[n];
                    }

                    // process [smoothing]: integral
                    if (p.integral > 0) {
                        bars[n] = bars_mem[n] * integral + bars[n];
                        bars_mem[n] = bars[n];

                        int diff = height - bars[n];
                        if (diff < 0)
                            diff = 0;
                        double div = 1 / (diff + 1);
                        // bars[n] = bars[n] - pow(div, 10) * (height + 1);
                        bars_mem[n] = bars_mem[n] * (1 - div / 20);
                    }
#ifndef NDEBUG
                    mvprintw(n, 0, "%d: f:%f->%f (%d->%d), eq:\
						%15e, peak:%d \n",
                             n, cut_off_frequency[n], cut_off_frequency[n + 1],
                             FFTbuffer_lower_cut_off[n], FFTbuffer_upper_cut_off[n], eq[n],
                             bars[n]);

                    if (bars[n] < minvalue) {
                        minvalue = bars[n];
                        debug("min value: %d\n", minvalue); // checking maxvalue 10000
                    }
                    if (bars[n] > maxvalue) {
                        maxvalue = bars[n];
                    }
                    if (bars[n] < 0) {
                        debug("negative bar value!! %d\n", bars[n]);
                        //    exit(EXIT_FAILURE); // Can't happen.
                    }

#endif

                    // zero values causes divided by zero segfault (if not raw)
                    if (output_mode != OUTPUT_RAW && bars[n] < 1)
                        bars[n] = 1;

                    // autmatic sens adjustment
                    if (p.autosens) {
                        if (bars[n] > height && senselow) {
                            p.sens = p.sens * 0.98;
                            senselow = false;
                        }
                    }
                }

                if (p.autosens && !silence && senselow)
                    p.sens = p.sens * 1.001;

#ifndef NDEBUG
                mvprintw(n + 1, 0, "sensitivity %.10e", p.sens);
                mvprintw(n + 2, 0, "min value: %d\n", minvalue); // checking maxvalue 10000
                mvprintw(n + 3, 0, "max value: %d\n", maxvalue); // checking maxvalue 10000
#endif

// output: draw processed input
#ifdef NDEBUG
                switch (output_mode) {
                case OUTPUT_NCURSES:
#ifdef NCURSES
                    rc = draw_terminal_ncurses(inAtty, lines, width, number_of_bars, p.bar_width,
                                               p.bar_spacing, rest, bars, previous_frame,
                                               p.gradient);
                    break;
#endif
                case OUTPUT_NONCURSES:
                    rc = draw_terminal_noncurses(inAtty, lines, width, number_of_bars, p.bar_width,
                                                 p.bar_spacing, rest, bars, previous_frame);
                    break;
                case OUTPUT_RAW:
                    rc = print_raw_out(number_of_bars, fp, p.is_bin, p.bit_format, p.ascii_range,
                                       p.bar_delim, p.frame_delim, bars);
                    break;

                default:
                    exit(EXIT_FAILURE); // Can't happen.
                }

                // terminal has been resized breaking to recalibrating values
                if (rc == -1)
                    resizeTerminal = true;

#endif

                memcpy(previous_frame, bars, 256 * sizeof(int));

                // checking if audio thread has exited unexpectedly
                if (audio.terminate == 1) {
                    cleanup();
                    fprintf(stderr, "Audio thread exited unexpectedly. %s\n", audio.error_message);
                    exit(EXIT_FAILURE);
                }

                nanosleep(&req, NULL);
            } // resize terminal

        } // reloading config
        req.tv_sec = 0;
        req.tv_nsec = 100; // waiting some time to make shure audio is ready
        nanosleep(&req, NULL);

        //**telling audio thread to terminate**//
        audio.terminate = 1;
        pthread_join(p_thread, NULL);

        if (p.userEQ_enabled)
            free(p.userEQ);
        if (sourceIsAuto)
            free(audio.source);

        fftw_free(audio.in_bass_r);
        fftw_free(audio.in_bass_l);
        fftw_free(out_bass_r);
        fftw_free(out_bass_l);
        fftw_destroy_plan(p_bass_l);
        fftw_destroy_plan(p_bass_r);

        fftw_free(audio.in_mid_r);
        fftw_free(audio.in_mid_l);
        fftw_free(out_mid_r);
        fftw_free(out_mid_l);
        fftw_destroy_plan(p_mid_l);
        fftw_destroy_plan(p_mid_r);

        fftw_free(audio.in_treble_r);
        fftw_free(audio.in_treble_l);
        fftw_free(out_treble_r);
        fftw_free(out_treble_l);
        fftw_destroy_plan(p_treble_l);
        fftw_destroy_plan(p_treble_r);

        cleanup();

        // fclose(fp);
    }
}
