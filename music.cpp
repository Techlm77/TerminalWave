#include <iostream>
#include <cstdlib>
#include <sys/ioctl.h>

#if __has_include(<filesystem>)
  #include <filesystem>
  namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
  #include <experimental/filesystem>
  namespace fs = std::experimental::filesystem;
#else
  #error "No filesystem support"
#endif

#include <vector>
#include <string>
#include <deque>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ncurses.h>
#include <mpg123.h>
#include <portaudio.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <csignal>
#include <fftw3.h>
#include <chrono>

#define BUFFER_SIZE 8192
#define FRAMES_PER_BUFFER 512
#define FFT_SIZE 1024

enum VisualizationMode { WAVEFORM = 1, SPECTRUM = 2 };

static int clampi(int v, int lo, int hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

static std::string ellipsize_middle(const std::string& s, int maxw) {
    if (maxw <= 0) return "";
    if ((int)s.size() <= maxw) return s;
    if (maxw <= 3) return s.substr(0, maxw);
    int left = (maxw - 3) / 2;
    int right = maxw - 3 - left;
    return s.substr(0, left) + "..." + s.substr((int)s.size() - right);
}

static std::string ellipsize_end(const std::string& s, int maxw) {
    if (maxw <= 0) return "";
    if ((int)s.size() <= maxw) return s;
    if (maxw <= 3) return s.substr(0, maxw);
    return s.substr(0, maxw - 3) + "...";
}

static void safe_waddnstr(WINDOW* w, int y, int x, const std::string& s, int n) {
    if (!w) return;
    mvwaddnstr(w, y, x, s.c_str(), n);
}

std::atomic<bool> shouldQuit(false);
std::atomic<bool> stopTrack(false);
std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<int>  seekCommand(0);
std::atomic<VisualizationMode> visMode(WAVEFORM);

std::mutex playlistMutex;
std::deque<std::string> playlist;
std::condition_variable playlistCV;

std::mutex pauseMutex;
std::condition_variable pauseCV;

std::atomic<bool> needResize(false);

WINDOW* navWin  = nullptr;
WINDOW* infoWin = nullptr;
WINDOW* waveWin = nullptr;
WINDOW* statusWin = nullptr;

int totalH = 0, totalW = 0, halfH = 0, halfW = 0;
int listOffset = 0;

struct RenderState {
    std::string file;
    double curSec = 0.0;
    double totalSec = 0.0;
    VisualizationMode mode = WAVEFORM;
    bool paused = false;
    std::vector<int16_t> mono;
    std::vector<double> magnitudes;
};

std::mutex renderMutex;
RenderState renderState;
std::atomic<bool> renderDirty(false);

static void close_tui() {
    if (statusWin) { delwin(statusWin); statusWin = nullptr; }
    if (waveWin) { delwin(waveWin); waveWin = nullptr; }
    if (infoWin) { delwin(infoWin); infoWin = nullptr; }
    if (navWin)  { delwin(navWin);  navWin  = nullptr; }
    endwin();
}

static void draw_border(WINDOW* w, int colorPair, bool bold) {
    if (!w) return;
    if (bold) wattron(w, A_BOLD);
    wattron(w, COLOR_PAIR(colorPair));
    wborder(w, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
    wattroff(w, COLOR_PAIR(colorPair));
    if (bold) wattroff(w, A_BOLD);
}

static void draw_title(WINDOW* w, const std::string& title, int colorPair) {
    if (!w) return;
    int h, ww;
    getmaxyx(w, h, ww);
    std::string t = " " + title + " ";
    int x = 2;
    int maxw = ww - 4;
    if (maxw <= 0) return;
    t = ellipsize_end(t, maxw);
    wattron(w, COLOR_PAIR(colorPair) | A_BOLD);
    mvwaddnstr(w, 0, x, t.c_str(), maxw);
    wattroff(w, COLOR_PAIR(colorPair) | A_BOLD);
}

static void handle_resize() {
    int h = 0, w = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 || ws.ws_col == 0) {
        getmaxyx(stdscr, h, w);
    } else {
        h = ws.ws_row;
        w = ws.ws_col;
    }

    totalH = h;
    totalW = w;
    if (totalH < 12 || totalW < 30) return;

    halfH = totalH / 2;
    halfW = totalW / 2;

    resizeterm(totalH, totalW);
    clear();
    refresh();

    if (statusWin) { delwin(statusWin); statusWin = nullptr; }
    if (navWin)  { delwin(navWin);  navWin  = nullptr; }
    if (infoWin) { delwin(infoWin); infoWin = nullptr; }
    if (waveWin) { delwin(waveWin); waveWin = nullptr; }

    int statusH = 1;
    int topH = halfH;
    int bottomH = totalH - topH - statusH;
    if (bottomH < 3) bottomH = 3;

    navWin  = newwin(topH, halfW, 0, 0);
    infoWin = newwin(topH, totalW - halfW, 0, halfW);
    waveWin = newwin(bottomH, totalW, topH, 0);
    statusWin = newwin(statusH, totalW, totalH - statusH, 0);

    werase(navWin); werase(infoWin); werase(waveWin); werase(statusWin);
    draw_border(navWin, 2, false);
    draw_border(infoWin, 2, false);
    draw_border(waveWin, 2, false);

    draw_title(navWin, "Browser", 1);
    draw_title(infoWin, "Now Playing", 1);
    draw_title(waveWin, "Visualizer", 1);

    wrefresh(navWin);
    wrefresh(infoWin);
    wrefresh(waveWin);
    wrefresh(statusWin);
}

static void init_tui() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, true);
    timeout(30);

    if (!has_colors()) {
        endwin();
        std::cerr << "Error: Terminal does not support colors.\n";
        std::exit(1);
    }

    start_color();
    use_default_colors();

    init_pair(1, COLOR_CYAN,   -1);
    init_pair(2, COLOR_WHITE,  -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_GREEN,  -1);
    init_pair(5, COLOR_MAGENTA,-1);
    init_pair(6, COLOR_RED,    -1);

    mousemask(0, nullptr);

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 || ws.ws_col == 0) {
        getmaxyx(stdscr, totalH, totalW);
    } else {
        totalH = ws.ws_row;
        totalW = ws.ws_col;
    }

    if (totalH < 12 || totalW < 30) {
        endwin();
        std::cerr << "Error: Terminal window too small.\n";
        std::exit(1);
    }

    halfH = totalH / 2;
    halfW = totalW / 2;

    int statusH = 1;
    int topH = halfH;
    int bottomH = totalH - topH - statusH;
    if (bottomH < 3) bottomH = 3;

    navWin  = newwin(topH, halfW, 0, 0);
    infoWin = newwin(topH, totalW - halfW, 0, halfW);
    waveWin = newwin(bottomH, totalW, topH, 0);
    statusWin = newwin(statusH, totalW, totalH - statusH, 0);

    werase(navWin); werase(infoWin); werase(waveWin); werase(statusWin);

    draw_border(navWin, 2, false);
    draw_border(infoWin, 2, false);
    draw_border(waveWin, 2, false);

    draw_title(navWin, "Browser", 1);
    draw_title(infoWin, "Now Playing", 1);
    draw_title(waveWin, "Visualizer", 1);

    refresh();
    wrefresh(navWin);
    wrefresh(infoWin);
    wrefresh(waveWin);
    wrefresh(statusWin);
}

static std::vector<fs::directory_entry> list_directory(const fs::path& p) {
    std::vector<fs::directory_entry> entries;
    try {
        for (auto& x : fs::directory_iterator(p)) entries.push_back(x);
    } catch (const std::exception& e) {
        std::cerr << "Error accessing directory: " << e.what() << "\n";
    }

    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        bool ad = a.is_directory();
        bool bd = b.is_directory();
        if (ad != bd) return ad > bd;
        return a.path().filename().string() < b.path().filename().string();
    });

    return entries;
}

static void format_time(double sec, char* buf, size_t bufsize) {
    long t = static_cast<long>(sec);
    if (t < 0) t = 0;
    long mm = t / 60, ss = t % 60;
    std::snprintf(buf, bufsize, "%02ld:%02ld", mm, ss);
}

static void draw_status_bar(const fs::path& currentDir) {
    if (!statusWin) return;

    werase(statusWin);

    int qsz = 0;
    {
        std::lock_guard<std::mutex> lk(playlistMutex);
        qsz = (int)playlist.size();
    }

    bool playing = isPlaying.load();
    bool paused = isPaused.load();
    VisualizationMode m = visMode.load();

    std::string left = " q:quit  Enter:open/add  a:queue mp3  s:skip  x:stop  p:pause  1/2:mode  \u2190/\u2192:seek ";
    std::string right;

    std::string dir = currentDir.string();
    right += "  Dir: " + dir;
    right += "  Queue: " + std::to_string(qsz);
    right += "  Mode: " + std::string((m == WAVEFORM) ? "Wave" : "Spec");
    right += "  State: " + std::string(!playing ? "Idle" : (paused ? "Paused" : "Play"));

    int h, w;
    getmaxyx(statusWin, h, w);

    int avail = w;
    std::string r = ellipsize_middle(right, clampi(avail / 2, 10, avail));
    int rlen = (int)r.size();
    int lmax = w - rlen - 1;
    if (lmax < 0) lmax = 0;

    std::string l = ellipsize_end(left, lmax);

    wattron(statusWin, A_REVERSE);
    if (playing) wattron(statusWin, COLOR_PAIR(paused ? 3 : 4));
    else wattron(statusWin, COLOR_PAIR(5));

    mvwaddnstr(statusWin, 0, 0, l.c_str(), lmax);
    if (rlen > 0) {
        mvwaddnstr(statusWin, 0, w - rlen, r.c_str(), rlen);
    }

    wattroff(statusWin, COLOR_PAIR(paused ? 3 : 4));
    wattroff(statusWin, COLOR_PAIR(5));
    wattroff(statusWin, A_REVERSE);

    wrefresh(statusWin);
}

static void draw_info(const std::string& filepath, double currentSec, double totalSec, VisualizationMode mode, bool paused) {
    if (!infoWin) return;

    werase(infoWin);
    draw_border(infoWin, 2, false);
    draw_title(infoWin, "Now Playing", 1);

    int h, w;
    getmaxyx(infoWin, h, w);
    int innerW = w - 4;
    if (innerW < 10 || h < 6) { wrefresh(infoWin); return; }

    std::string title = "Idle";
    if (!filepath.empty()) {
        fs::path p(filepath);
        title = p.filename().string();
    }

    std::string state = paused ? "PAUSED" : (isPlaying.load() ? "PLAYING" : "IDLE");
    int statePair = paused ? 3 : (isPlaying.load() ? 4 : 5);

    wattron(infoWin, COLOR_PAIR(statePair) | A_BOLD);
    mvwaddnstr(infoWin, 1, 2, state.c_str(), innerW);
    wattroff(infoWin, COLOR_PAIR(statePair) | A_BOLD);

    std::string tline = ellipsize_end(title, innerW);
    wattron(infoWin, COLOR_PAIR(2) | A_BOLD);
    mvwaddnstr(infoWin, 2, 2, tline.c_str(), innerW);
    wattroff(infoWin, COLOR_PAIR(2) | A_BOLD);

    char cb[16], tb[16];
    format_time(currentSec, cb, sizeof(cb));
    format_time(totalSec, tb, sizeof(tb));
    std::string timeLine = std::string(cb) + " / " + std::string(tb);

    double progress = (totalSec > 0.0) ? (currentSec / totalSec) : 0.0;
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;

    int barW = innerW;
    if (barW < 10) barW = 10;
    int filled = (int)std::round(progress * (double)barW);
    filled = clampi(filled, 0, barW);

    int barY = 4;
    if (barY < h - 2) {
        wattron(infoWin, COLOR_PAIR(3));
        mvwaddnstr(infoWin, barY - 1, 2, timeLine.c_str(), innerW);
        wattroff(infoWin, COLOR_PAIR(3));

        for (int i = 0; i < barW; i++) {
            if (i < filled) wattron(infoWin, COLOR_PAIR(4) | A_BOLD);
            else wattron(infoWin, COLOR_PAIR(2));
            mvwaddch(infoWin, barY, 2 + i, (i < filled) ? '=' : '-');
            if (i < filled) wattroff(infoWin, COLOR_PAIR(4) | A_BOLD);
            else wattroff(infoWin, COLOR_PAIR(2));
        }

        std::string modeLine = std::string("Visualizer: ") + ((mode == WAVEFORM) ? "Waveform" : "Spectrum");
        wattron(infoWin, COLOR_PAIR(5));
        mvwaddnstr(infoWin, barY + 1, 2, modeLine.c_str(), innerW);
        wattroff(infoWin, COLOR_PAIR(5));
    }

    wrefresh(infoWin);
}

static void draw_scrollbar(WINDOW* w, int contentTopY, int contentBottomY, int totalItems, int firstIndex, int visibleItems) {
    if (!w) return;
    int h, ww;
    getmaxyx(w, h, ww);
    if (ww < 3) return;
    if (totalItems <= visibleItems || visibleItems <= 0) return;

    int sbX = ww - 2;
    int sbTop = contentTopY;
    int sbBottom = contentBottomY;
    int sbH = sbBottom - sbTop + 1;
    if (sbH <= 0) return;

    double fracTop = (double)firstIndex / (double)std::max(1, totalItems);
    double fracVis = (double)visibleItems / (double)std::max(1, totalItems);

    int thumbH = clampi((int)std::round(fracVis * sbH), 1, sbH);
    int thumbY = sbTop + clampi((int)std::round(fracTop * sbH), 0, sbH - thumbH);

    wattron(w, COLOR_PAIR(2));
    for (int y = sbTop; y <= sbBottom; y++) mvwaddch(w, y, sbX, ACS_VLINE);
    wattroff(w, COLOR_PAIR(2));

    wattron(w, COLOR_PAIR(1) | A_BOLD);
    for (int y = thumbY; y < thumbY + thumbH; y++) mvwaddch(w, y, sbX, ' ');
    wattroff(w, COLOR_PAIR(1) | A_BOLD);
}

static void draw_navigation(const fs::path& current, const std::vector<fs::directory_entry>& dirList, int highlight) {
    if (!navWin) return;

    werase(navWin);
    draw_border(navWin, 2, false);

    std::string title = "Browser";
    draw_title(navWin, title, 1);

    int navH, navW;
    getmaxyx(navWin, navH, navW);
    int innerH = navH - 2;
    int innerW = navW - 4;
    if (innerH <= 0 || innerW <= 0) { wrefresh(navWin); return; }

    int headerY = 1;
    std::string dirLine = ellipsize_middle(current.string(), innerW);
    wattron(navWin, COLOR_PAIR(5));
    mvwaddnstr(navWin, headerY, 2, dirLine.c_str(), innerW);
    wattroff(navWin, COLOR_PAIR(5));

    int contentTop = 3;
    int contentBottom = navH - 2;
    int linesForItems = contentBottom - contentTop + 1;
    if (linesForItems < 1) { wrefresh(navWin); return; }

    int totalItems = (int)dirList.size() + 1;
    highlight = clampi(highlight, 0, std::max(0, totalItems - 1));

    if (highlight < listOffset) listOffset = highlight;
    if (highlight >= listOffset + linesForItems) listOffset = highlight - linesForItems + 1;
    listOffset = clampi(listOffset, 0, std::max(0, totalItems - linesForItems));

    int nameW = innerW - 6;
    if (nameW < 8) nameW = innerW;

    for (int row = 0; row < linesForItems; row++) {
        int idx = listOffset + row;
        if (idx >= totalItems) break;

        int y = contentTop + row;
        bool selected = (idx == highlight);

        if (selected) wattron(navWin, A_REVERSE | A_BOLD);

        if (idx == 0) {
            wattron(navWin, COLOR_PAIR(3) | (selected ? A_REVERSE : 0));
            mvwaddnstr(navWin, y, 2, " ..", innerW);
            wattroff(navWin, COLOR_PAIR(3) | (selected ? A_REVERSE : 0));
        } else {
            int realIndex = idx - 1;
            auto& e = dirList[realIndex];
            bool isDir = e.is_directory();
            std::string nm = e.path().filename().string();
            std::string icon = isDir ? " [D] " : " [F] ";
            std::string line = icon + nm + (isDir ? "/" : "");
            line = ellipsize_end(line, innerW);

            int pair = isDir ? 1 : 2;
            wattron(navWin, COLOR_PAIR(pair) | (selected ? A_REVERSE : 0));
            mvwaddnstr(navWin, y, 2, line.c_str(), innerW);
            wattroff(navWin, COLOR_PAIR(pair) | (selected ? A_REVERSE : 0));
        }

        if (selected) wattroff(navWin, A_REVERSE | A_BOLD);
    }

    draw_scrollbar(navWin, contentTop, contentBottom, totalItems, listOffset, linesForItems);

    wrefresh(navWin);
}

static void draw_visualization(const std::vector<int16_t>& mono, const std::vector<double>& mags, VisualizationMode mode) {
    if (!waveWin) return;

    int h, w;
    getmaxyx(waveWin, h, w);
    if (h < 5 || w < 20) {
        werase(waveWin);
        draw_border(waveWin, 2, false);
        draw_title(waveWin, "Visualizer", 1);
        wrefresh(waveWin);
        return;
    }

    werase(waveWin);
    draw_border(waveWin, 2, false);

    std::string vt = std::string("Visualizer - ") + ((mode == WAVEFORM) ? "Waveform" : "Spectrum");
    draw_title(waveWin, vt, 1);

    int plotTop = 2;
    int plotBottom = h - 2;
    int plotLeft = 1;
    int plotRight = w - 2;

    int plotH = plotBottom - plotTop + 1;
    int plotW = plotRight - plotLeft + 1;
    if (plotH <= 0 || plotW <= 0) { wrefresh(waveWin); return; }

    int midY = plotTop + plotH / 2;

    wattron(waveWin, COLOR_PAIR(5));
    for (int x = plotLeft; x <= plotRight; x++) mvwaddch(waveWin, midY, x, '.');
    wattroff(waveWin, COLOR_PAIR(5));

    if (mode == WAVEFORM) {
        if (!mono.empty()) {
            for (int x = 0; x < plotW; x++) {
                int idx = (int)std::round((double)x / (double)std::max(1, plotW - 1) * (double)(mono.size() - 1));
                idx = clampi(idx, 0, (int)mono.size() - 1);
                double v = (double)mono[idx] / 32768.0;
                int yOff = (int)std::round(v * (double)(plotH / 2));
                int y = clampi(midY - yOff, plotTop, plotBottom);
                wattron(waveWin, COLOR_PAIR(4) | A_BOLD);
                mvwaddch(waveWin, y, plotLeft + x, '*');
                wattroff(waveWin, COLOR_PAIR(4) | A_BOLD);
            }
        } else {
            std::string msg = "No data";
            wattron(waveWin, COLOR_PAIR(3));
            mvwaddnstr(waveWin, midY, plotLeft + clampi((plotW - (int)msg.size()) / 2, 0, plotW - 1), msg.c_str(), plotW);
            wattroff(waveWin, COLOR_PAIR(3));
        }
    } else {
        if (!mags.empty()) {
            double maxMag = 1e-12;
            for (double v : mags) if (v > maxMag) maxMag = v;

            int bins = (int)mags.size();
            int bars = plotW;
            int binsPerBar = std::max(1, bins / std::max(1, bars));

            for (int x = 0; x < bars; x++) {
                int start = x * binsPerBar;
                int end = std::min(start + binsPerBar, bins);
                if (start >= end) break;

                double sum = 0.0;
                for (int i = start; i < end; i++) sum += mags[i];
                double avg = sum / (double)(end - start);

                double ratio = std::log(avg + 1.0) / std::log(maxMag + 1.0);
                if (ratio < 0.0) ratio = 0.0;
                if (ratio > 1.0) ratio = 1.0;

                int bh = (int)std::round(ratio * (double)plotH);
                bh = clampi(bh, 0, plotH);

                for (int yy = 0; yy < bh; yy++) {
                    int y = plotBottom - yy;
                    wattron(waveWin, COLOR_PAIR(4) | A_BOLD);
                    mvwaddch(waveWin, y, plotLeft + x, '|');
                    wattroff(waveWin, COLOR_PAIR(4) | A_BOLD);
                }
            }
        } else {
            std::string msg = "No spectrum";
            wattron(waveWin, COLOR_PAIR(3));
            mvwaddnstr(waveWin, midY, plotLeft + clampi((plotW - (int)msg.size()) / 2, 0, plotW - 1), msg.c_str(), plotW);
            wattroff(waveWin, COLOR_PAIR(3));
        }
    }

    wrefresh(waveWin);
}

static bool play_file(const std::string& path) {
    std::freopen("/dev/null", "w", stderr);

    if (mpg123_init() != MPG123_OK) return false;

    mpg123_handle* mh = mpg123_new(nullptr, nullptr);
    if (!mh) { mpg123_exit(); return false; }

    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);

    off_t length = mpg123_length(mh);
    double totalSec = (length > 0) ? ((double)length / (double)rate) : 0.0;

    if (Pa_Initialize() != paNoError) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    PaStreamParameters out;
    out.device = Pa_GetDefaultOutputDevice();
    if (out.device == paNoDevice) {
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    out.channelCount = channels;
    out.sampleFormat = paInt16;
    out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    if (Pa_OpenStream(&stream, nullptr, &out, rate, FRAMES_PER_BUFFER, paClipOff, nullptr, nullptr) != paNoError) {
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    if (Pa_StartStream(stream) != paNoError) {
        Pa_CloseStream(stream);
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    isPlaying.store(true);
    isPaused.store(false);
    seekCommand.store(0);

    double* fftIn = (double*)fftw_malloc(sizeof(double) * FFT_SIZE);
    fftw_complex* fftOut = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * (FFT_SIZE / 2 + 1));
    if (!fftIn || !fftOut) {
        if (fftIn) fftw_free(fftIn);
        if (fftOut) fftw_free(fftOut);
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        isPlaying.store(false);
        return false;
    }

    fftw_plan fftPlan = fftw_plan_dft_r2c_1d(FFT_SIZE, fftIn, fftOut, FFTW_MEASURE);
    if (!fftPlan) {
        fftw_free(fftIn);
        fftw_free(fftOut);
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        isPlaying.store(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(renderMutex);
        renderState.file = path;
        renderState.curSec = 0.0;
        renderState.totalSec = totalSec;
        renderState.mode = visMode.load();
        renderState.paused = false;
        renderState.mono.assign(FFT_SIZE, 0);
        renderState.magnitudes.clear();
    }
    renderDirty.store(true, std::memory_order_release);

    unsigned char buffer[BUFFER_SIZE];
    bool wasPaused = false;
    double currentSec = 0.0;

    while (!shouldQuit.load() && !stopTrack.load()) {
        if (isPaused.load()) {
            if (!wasPaused) { Pa_StopStream(stream); wasPaused = true; }
            {
                std::lock_guard<std::mutex> lk(renderMutex);
                renderState.paused = true;
            }
            renderDirty.store(true, std::memory_order_release);

            std::unique_lock<std::mutex> lock(pauseMutex);
            pauseCV.wait(lock, [] { return !isPaused.load() || shouldQuit.load() || stopTrack.load(); });

            if (!isPaused.load() && Pa_IsStreamStopped(stream) == 1) Pa_StartStream(stream);
            wasPaused = false;

            {
                std::lock_guard<std::mutex> lk(renderMutex);
                renderState.paused = false;
            }
            renderDirty.store(true, std::memory_order_release);

            if (shouldQuit.load() || stopTrack.load()) break;
        }

        int cmdSec = seekCommand.exchange(0);
        if (cmdSec != 0) {
            off_t curPos = mpg123_tell(mh);
            if (curPos < 0) curPos = 0;

            off_t newPos = curPos + (off_t)cmdSec * (off_t)rate;
            if (newPos < 0) newPos = 0;
            if (length > 0 && newPos > length) newPos = length;

            mpg123_seek(mh, newPos, SEEK_SET);
        }

        size_t done = 0;
        int ret = mpg123_read(mh, buffer, BUFFER_SIZE, &done);
        if (ret == MPG123_DONE || ret != MPG123_OK) break;
        if (done == 0) continue;

        int frames = (int)(done / (channels * (int)sizeof(int16_t)));
        if (frames <= 0) continue;

        if (Pa_WriteStream(stream, buffer, frames) != paNoError) break;

        off_t curSamp = mpg123_tell(mh);
        if (curSamp >= 0) currentSec = (double)curSamp / (double)rate;

        int16_t* samples = reinterpret_cast<int16_t*>(buffer);
        int totalFramesInChunk = (int)(done / (channels * (int)sizeof(int16_t)));

        std::vector<int16_t> monoLocal(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) {
            int src = (int)std::round((double)i / (double)(FFT_SIZE - 1) * (double)std::max(0, totalFramesInChunk - 1));
            src = clampi(src, 0, std::max(0, totalFramesInChunk - 1));
            monoLocal[i] = samples[src * channels];
        }

        VisualizationMode modeLocal = visMode.load();
        std::vector<double> magsLocal;

        if (modeLocal == SPECTRUM) {
            for (int i = 0; i < FFT_SIZE; i++) fftIn[i] = (double)monoLocal[i] / 32768.0;
            fftw_execute(fftPlan);

            magsLocal.assign(FFT_SIZE / 2, 0.0);
            for (int i = 0; i < FFT_SIZE / 2; i++) {
                double re = fftOut[i][0], im = fftOut[i][1];
                magsLocal[i] = std::sqrt(re * re + im * im);
            }
        }

        {
            std::lock_guard<std::mutex> lk(renderMutex);
            renderState.file = path;
            renderState.curSec = currentSec;
            renderState.totalSec = totalSec;
            renderState.mode = modeLocal;
            renderState.paused = isPaused.load();
            renderState.mono = std::move(monoLocal);
            renderState.magnitudes = std::move(magsLocal);
        }
        renderDirty.store(true, std::memory_order_release);
    }

    fftw_destroy_plan(fftPlan);
    fftw_free(fftIn);
    fftw_free(fftOut);

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    isPlaying.store(false);

    {
        std::lock_guard<std::mutex> lk(renderMutex);
        renderState.curSec = 0.0;
        renderState.totalSec = 0.0;
        renderState.paused = false;
        renderState.mono.clear();
        renderState.magnitudes.clear();
    }
    renderDirty.store(true, std::memory_order_release);

    return true;
}

static void audio_thread() {
    while (!shouldQuit.load()) {
        std::string nextPath;
        {
            std::unique_lock<std::mutex> lock(playlistMutex);
            playlistCV.wait(lock, [] { return shouldQuit.load() || !playlist.empty(); });
            if (shouldQuit.load()) break;
            if (!playlist.empty()) {
                nextPath = playlist.front();
                playlist.pop_front();
            } else {
                continue;
            }
        }

        stopTrack.store(false);
        play_file(nextPath);

        if (shouldQuit.load()) break;
    }
    isPlaying.store(false);
}

static void on_resize(int) {
    needResize.store(true);
}

static void handle_sigint(int) {
    shouldQuit.store(true);
    playlistCV.notify_all();
    pauseCV.notify_all();
}

int main() {
    std::signal(SIGWINCH, on_resize);
    std::signal(SIGINT, handle_sigint);

    init_tui();

    const char* homeEnv = std::getenv("HOME");
    fs::path currentDir = homeEnv ? fs::path(homeEnv) : fs::current_path();
    auto dirList = list_directory(currentDir);

    int highlight = 0;
    bool redrawNav = true;

    std::thread at(audio_thread);

    auto lastRender = std::chrono::steady_clock::now();

    while (!shouldQuit.load()) {
        if (needResize.load()) {
            needResize.store(false);
            handle_resize();
            redrawNav = true;
        }

        if (!navWin || !infoWin || !waveWin || !statusWin) continue;

        if (redrawNav) {
            draw_navigation(currentDir, dirList, highlight);
            redrawNav = false;
        }

        bool shouldRenderNow = false;
        if (renderDirty.exchange(false, std::memory_order_acq_rel)) {
            shouldRenderNow = true;
        } else {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRender).count();
            if (isPlaying.load() && ms >= 50) shouldRenderNow = true;
        }

        if (shouldRenderNow) {
            RenderState snap;
            {
                std::lock_guard<std::mutex> lk(renderMutex);
                snap = renderState;
            }
            draw_info(snap.file, snap.curSec, snap.totalSec, snap.mode, snap.paused);
            draw_visualization(snap.mono, snap.magnitudes, snap.mode);
            lastRender = std::chrono::steady_clock::now();
        }

        draw_status_bar(currentDir);

        int c = getch();
        if (c == ERR) {
        } else if (c == 'q' || c == 'Q') {
            shouldQuit.store(true);
            playlistCV.notify_all();
            pauseCV.notify_all();
            break;
        } else if (c == KEY_UP) {
            if (highlight > 0) { highlight--; redrawNav = true; }
        } else if (c == KEY_DOWN) {
            int totalItems = (int)dirList.size() + 1;
            if (highlight < totalItems - 1) { highlight++; redrawNav = true; }
        } else if (c == KEY_LEFT) {
            seekCommand.store(-5);
        } else if (c == KEY_RIGHT) {
            seekCommand.store(5);
        } else if (c == 'p' || c == 'P') {
            bool newPaused = !isPaused.load();
            isPaused.store(newPaused);
            if (!newPaused) pauseCV.notify_one();
        } else if (c == '1') {
            visMode.store(WAVEFORM);
            renderDirty.store(true);
        } else if (c == '2') {
            visMode.store(SPECTRUM);
            renderDirty.store(true);
        } else if (c == '\n') {
            if (highlight == 0) {
                if (currentDir.has_parent_path()) {
                    currentDir = currentDir.parent_path();
                    dirList = list_directory(currentDir);
                    highlight = 0;
                    listOffset = 0;
                    redrawNav = true;
                }
            } else {
                int realIndex = highlight - 1;
                if (realIndex >= 0 && realIndex < (int)dirList.size()) {
                    fs::directory_entry sel = dirList[realIndex];
                    if (sel.is_directory()) {
                        currentDir = sel.path();
                        dirList = list_directory(currentDir);
                        highlight = 0;
                        listOffset = 0;
                        redrawNav = true;
                    } else {
                        std::string ext = sel.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".mp3") {
                            {
                                std::lock_guard<std::mutex> lk(playlistMutex);
                                playlist.clear();
                                playlist.push_back(sel.path().string());
                            }
                            stopTrack.store(true);
                            isPaused.store(false);
                            pauseCV.notify_all();
                            playlistCV.notify_one();
                        }
                    }
                }
            }
        } else if (c == 'a' || c == 'A') {
            {
                std::lock_guard<std::mutex> lk(playlistMutex);
                for (auto& e : dirList) {
                    if (!e.is_directory()) {
                        std::string ex = e.path().extension().string();
                        std::transform(ex.begin(), ex.end(), ex.begin(), ::tolower);
                        if (ex == ".mp3") playlist.push_back(e.path().string());
                    }
                }
            }
            playlistCV.notify_one();
            redrawNav = true;
        } else if (c == 's' || c == 'S') {
            stopTrack.store(true);
            isPaused.store(false);
            pauseCV.notify_all();
        } else if (c == 'x' || c == 'X') {
            {
                std::lock_guard<std::mutex> lk(playlistMutex);
                playlist.clear();
            }
            stopTrack.store(true);
            isPaused.store(false);
            pauseCV.notify_all();
        }
    }

    if (at.joinable()) at.join();
    close_tui();
    return 0;
}
