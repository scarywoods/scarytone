// initialize

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <SDL2/SDL.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>

static Uint8 *audio_buffer = NULL;
static Uint32 audio_buffer_len = 0;
static Uint32 audio_buffer_pos = 0;

static float volume = 1.0f;
static int paused = 0;
static int running = 1;

static int sample_rate = 0;
static int channels = 0;
static int bytes_per_second = 0;

static struct termios orig_termios;
static char display_title[256] = {0};

void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void setup_terminal(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    if (paused || audio_buffer_pos >= audio_buffer_len) {
        SDL_memset(stream, 0, len);
        return;
    }

    Uint32 remaining = audio_buffer_len - audio_buffer_pos;
    Uint32 to_copy = (len > remaining) ? remaining : len;

    SDL_memcpy(stream, audio_buffer + audio_buffer_pos, to_copy);

    int16_t *samples = (int16_t *)stream;
    int count = to_copy / 2;
    for (int i = 0; i < count; i++) {
        float s = samples[i] * volume;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        samples[i] = (int16_t)s;
    }

    audio_buffer_pos += to_copy;

    if (to_copy < len) {
        SDL_memset(stream + to_copy, 0, len - to_copy);
    }
}


// rendering
void *ui_thread(void *arg) {
    const int bar_width = 37;

    while (running) {

        char c;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q') {
                running = 0;
                break;
            } else if (c == ' ') {
                paused = !paused;
                SDL_PauseAudio(paused);
            } else if (c == '\033') {
                char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                    read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[0] == '[') {
                        if (seq[1] == 'C') {
                            int delta = 5 * bytes_per_second;
                            audio_buffer_pos = (audio_buffer_pos + delta > audio_buffer_len)
                            ? audio_buffer_len
                            : audio_buffer_pos + delta;
                        } else if (seq[1] == 'D') {
                            int delta = 5 * bytes_per_second;
                            audio_buffer_pos = (audio_buffer_pos < delta)
                            ? 0
                            : audio_buffer_pos - delta;
                        } else if (seq[1] == 'A') {
                            volume += 0.1f;
                            if (volume > 2.0f) volume = 2.0f;
                        } else if (seq[1] == 'B') {
                            volume -= 0.1f;
                            if (volume < 0.0f) volume = 0.0f;
                        }
                    }
                    }
            }
        }
        if (!running) break;

        float pos_sec = (float)audio_buffer_pos / (float)bytes_per_second;
        float total_sec = (float)audio_buffer_len / (float)bytes_per_second;
        if (total_sec < 0.001f) total_sec = 0.001f;

        float ratio = pos_sec / total_sec;
        if (ratio > 1.0f) ratio = 1.0f;
        int filled = (int)(ratio * bar_width);

        int finished = (audio_buffer_pos >= audio_buffer_len && audio_buffer_len > 0);

        printf("\033[H\033[J");

        printf("┌─────────────────────────────────────────┐\n");

        const char *icon = "PLAY";
        if (paused)        icon = "PAUSE";
        else if (finished) icon = "END";

        char full_title[256];
        snprintf(full_title, sizeof(full_title), "%s",
                 display_title[0] ? display_title : "(unknown)");

        char status_block[32];
        snprintf(status_block, sizeof(status_block), " | %s", icon);

        int status_len = strlen(status_block);

        int title_width = 41 - 11 - status_len;
        if (title_width < 0) title_width = 0;

        char title_cut[128];
        int full_len = strlen(full_title);
        if (full_len <= title_width) {
            snprintf(title_cut, sizeof(title_cut), "%-*s", title_width, full_title);
        } else {
            strncpy(title_cut, full_title, title_width);
            title_cut[title_width] = '\0';
        }

        char interior[128];
        snprintf(interior, sizeof(interior),
                 "Playing: %s%s", title_cut, status_block);

        printf("│ %s │\n", interior);

        printf("├─────────────────────────────────────────┤\n");

        char bar_line[64];
        int idx = 0;
        bar_line[idx++] = '[';
        for (int i = 0; i < bar_width; i++) {
            if (i < filled) bar_line[idx++] = '=';
            else if (i == filled) bar_line[idx++] = '>';
            else bar_line[idx++] = '-';
        }
        bar_line[idx++] = ']';
        bar_line[idx] = '\0';

        printf("│ %-39s │\n", bar_line);

        char status_line[128];

        int vol_percent = (int)(volume * 100.0f + 0.5f);
        if (vol_percent < 0) vol_percent = 0;
        if (vol_percent > 200) vol_percent = 200;

        char vol_label[16];
        if (vol_percent == 0) strcpy(vol_label, "MUTE");
        else if (vol_percent == 200) strcpy(vol_label, "LOUD");
        else snprintf(vol_label, sizeof(vol_label), "%d%%", vol_percent);

        int cur_m = (int)pos_sec / 60;
        int cur_s = (int)pos_sec % 60;
        int tot_m = (int)total_sec / 60;
        int tot_s = (int)total_sec % 60;

        snprintf(status_line, sizeof(status_line),
                 "Time: %d:%02d / %d:%02d  |       Volume: %s",
                 cur_m, cur_s, tot_m, tot_s, vol_label);

        printf("│ %-39s │\n", status_line);

        printf("└─────────────────────────────────────────┘\n");

        fflush(stdout);
        usleep(100000);
    }

    return NULL;
}

        int main(int argc, char **argv) {
            if (argc < 2) {
                fprintf(stderr, "Usage: %s <audiofile> [--flags]\n", argv[0]);
                return 1;
            }

            static char filepath[4096];
            filepath[0] = '\0';

            char *flags[64];
            int flags_count = 0;

            for (int i = 1; i < argc; i++) {
                if (strncmp(argv[i], "--", 2) == 0) {
                    if (flags_count < 64)
                        flags[flags_count++] = argv[i];
                } else {
                    strcat(filepath, argv[i]);
                    if (i < argc - 1)
                        strcat(filepath, " ");
                }
            }

            if (filepath[0] == '\0') {
                fprintf(stderr, "Error: No audio file path provided.\n");
                return 1;
            }

            const char *base = strrchr(filepath, '/');
            if (base) base++;
            else base = filepath;
            strncpy(display_title, base, sizeof(display_title) - 1);
            display_title[sizeof(display_title) - 1] = '\0';

            AVFormatContext *fmt = NULL;
            if (avformat_open_input(&fmt, filepath, NULL, NULL) < 0) {
                fprintf(stderr, "Could not open file: %s\n", filepath);
                return 1;
            }

            if (avformat_find_stream_info(fmt, NULL) < 0) {
                fprintf(stderr, "Could not find stream info\n");
                avformat_close_input(&fmt);
                return 1;
            }

            AVDictionaryEntry *tag = av_dict_get(fmt->metadata, "title", NULL, 0);
            if (tag && tag->value && tag->value[0]) {
                strncpy(display_title, tag->value, sizeof(display_title) - 1);
                display_title[sizeof(display_title) - 1] = '\0';
            }

            int audio_stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
            if (audio_stream < 0) {
                fprintf(stderr, "No audio stream found\n");
                avformat_close_input(&fmt);
                return 1;
            }

            AVCodecParameters *codecpar = fmt->streams[audio_stream]->codecpar;
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (!codec) {
                fprintf(stderr, "Unsupported codec\n");
                avformat_close_input(&fmt);
                return 1;
            }

            AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
            if (!codec_ctx) {
                fprintf(stderr, "Could not allocate codec context\n");
                avformat_close_input(&fmt);
                return 1;
            }

            if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
                fprintf(stderr, "Could not copy codec parameters\n");
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
                fprintf(stderr, "Could not open codec\n");
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            if (codec_ctx->ch_layout.nb_channels == 0) {
                if (codecpar->ch_layout.nb_channels > 0)
                    av_channel_layout_copy(&codec_ctx->ch_layout, &codecpar->ch_layout);
                else
                    av_channel_layout_default(&codec_ctx->ch_layout, 2);
            }

            sample_rate = codec_ctx->sample_rate;
            channels = codec_ctx->ch_layout.nb_channels;
            bytes_per_second = sample_rate * channels * 2;

            SwrContext *swr = swr_alloc();
            if (!swr) {
                fprintf(stderr, "Could not allocate SwrContext\n");
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            av_opt_set_chlayout(swr, "in_chlayout",  &codec_ctx->ch_layout, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &codec_ctx->ch_layout, 0);
            av_opt_set_int(swr, "in_sample_rate",    codec_ctx->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate",   codec_ctx->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt",  codec_ctx->sample_fmt, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

            if (swr_init(swr) < 0) {
                fprintf(stderr, "Could not initialize SwrContext\n");
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            if (SDL_Init(SDL_INIT_AUDIO) < 0) {
                fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            SDL_AudioSpec want, have;
            SDL_zero(want);
            want.freq = codec_ctx->sample_rate;
            want.format = AUDIO_S16SYS;
            want.channels = codec_ctx->ch_layout.nb_channels;
            want.samples = 4096;
            want.callback = audio_callback;

            if (SDL_OpenAudio(&want, &have) < 0) {
                fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
                SDL_Quit();
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            AVPacket *pkt = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            if (!pkt || !frame) {
                fprintf(stderr, "Could not allocate packet or frame\n");
                if (pkt) av_packet_free(&pkt);
                if (frame) av_frame_free(&frame);
                SDL_CloseAudio();
                SDL_Quit();
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }

            int buffer_size = 192000 * 20;
            audio_buffer = malloc(buffer_size);
            if (!audio_buffer) {
                fprintf(stderr, "Could not allocate audio buffer\n");
                av_frame_free(&frame);
                av_packet_free(&pkt);
                SDL_CloseAudio();
                SDL_Quit();
                swr_free(&swr);
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt);
                return 1;
            }
            audio_buffer_len = 0;
            audio_buffer_pos = 0;

            while (av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == audio_stream) {
                    if (avcodec_send_packet(codec_ctx, pkt) < 0) {
                        av_packet_unref(pkt);
                        continue;
                    }

                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        uint8_t *out_data[2] = {0};
                        int out_linesize = 0;

                        if (av_samples_alloc(out_data, &out_linesize,
                            codec_ctx->ch_layout.nb_channels,
                            frame->nb_samples,
                            AV_SAMPLE_FMT_S16, 0) < 0) {
                            continue;
                            }

                            int samples = swr_convert(swr,
                                                      out_data, frame->nb_samples,
                                                      (const uint8_t **)frame->data,
                                                      frame->nb_samples);

                            if (samples < 0) {
                                av_freep(&out_data[0]);
                                continue;
                            }

                            int bytes = samples * codec_ctx->ch_layout.nb_channels * 2;

                            if (audio_buffer_len + bytes < (Uint32)buffer_size) {
                                memcpy(audio_buffer + audio_buffer_len, out_data[0], bytes);
                                audio_buffer_len += bytes;
                            }

                            av_freep(&out_data[0]);
                    }
                }
                av_packet_unref(pkt);
            }

            setup_terminal();

            pthread_t ui;
            pthread_create(&ui, NULL, ui_thread, NULL);

            SDL_PauseAudio(0);

            while (running) {
                SDL_Delay(50);
            }

            running = 0;
            pthread_join(ui, NULL);

            printf("\n");

            SDL_CloseAudio();
            SDL_Quit();

            av_frame_free(&frame);
            av_packet_free(&pkt);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt);
            swr_free(&swr);
            free(audio_buffer);

            return 0;
        }
