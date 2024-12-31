#include <iostream>
#include <filesystem>
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
#include <fstream>
#include <fftw3.h>

namespace fs = std::filesystem;

// Constants
#define BUFFER_SIZE 8192
#define FRAMES_PER_BUFFER 512
#define FFT_SIZE 1024

enum VisualizationMode {
    WAVEFORM = 1,
    SPECTRUM = 2
};

// Atomic flags for thread-safe operations
std::atomic<bool> shouldQuit(false);
std::atomic<bool> stopTrack(false);
std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<int> seekCommand(0);
std::atomic<VisualizationMode> visMode(WAVEFORM);

// Playlist management
std::mutex playlistMutex;
std::deque<std::string> playlist;
std::condition_variable playlistCV;

// Window resize handling
std::atomic<bool> needResize(false);

// Pause handling
std::mutex pauseMutex;
std::condition_variable pauseCV;

// Ncurses windows
WINDOW* navWin = nullptr;
WINDOW* waveWin = nullptr;
int totalH = 0;
int totalW = 0;
int halfH = 0;

// Variables for scrolling
int listOffset = 0;

// Constants for layout
const int HEADER_LINES = 2; // "File Browser" and "[..]"
const int HELP_LINES = 3;   // Help instructions

// Structure to hold playback status
struct PlaybackStatus {
    std::string currentTrack;
    double currentSec;
    double totalSec;
    std::vector<int16_t> waveform;
    std::vector<double> spectrum;
};

std::mutex playbackMutex;
PlaybackStatus playbackStatus;

// Function to close the TUI
void close_tui() {
    if (waveWin) {
        delwin(waveWin);
        waveWin = nullptr;
    }

    if (navWin) {
        delwin(navWin);
        navWin = nullptr;
    }
    endwin();
}

// Function to reinitialize windows upon resize
void cleanup_and_reinit_windows() {
    endwin();
    refresh();
    clear();

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, true);
    if (!has_colors()) {
        endwin();
        std::cerr << "Terminal does not support color." << std::endl;
        exit(1);
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    timeout(0);
    mousemask(0, nullptr);
    getmaxyx(stdscr, totalH, totalW);
    halfH = totalH / 2;

    if (navWin) {
        wresize(navWin, halfH, totalW);
        mvwin(navWin, 0, 0);
    } else {
        navWin = newwin(halfH, totalW, 0, 0);
    }

    if (waveWin) {
        wresize(waveWin, totalH - halfH, totalW);
        mvwin(waveWin, halfH, 0);
    } else {
        waveWin = newwin(totalH - halfH, totalW, halfH, 0);
    }

    box(navWin, 0, 0);
    box(waveWin, 0, 0);
    wrefresh(navWin);
    wrefresh(waveWin);
}

// Function to initialize the TUI
void init_tui() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, true);
    if (!has_colors()) {
        endwin();
        std::cerr << "Terminal does not support color." << std::endl;
        exit(1);
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    timeout(0);
    mousemask(0, nullptr);
    getmaxyx(stdscr, totalH, totalW);
    halfH = totalH / 2;
    navWin = newwin(halfH, totalW, 0, 0);
    waveWin = newwin(totalH - halfH, totalW, halfH, 0);
    box(navWin, 0, 0);
    box(waveWin, 0, 0);
    wrefresh(navWin);
    wrefresh(waveWin);
}

// Function to list directory contents
std::vector<fs::directory_entry> list_directory(const fs::path& p) {
    std::vector<fs::directory_entry> entries;
    try {
        for (auto& x : fs::directory_iterator(p)) {
            entries.push_back(x);
        }
    } catch (...) {
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        if (a.is_directory() && !b.is_directory()) return true;
        if (!a.is_directory() && b.is_directory()) return false;
        return a.path().filename().string() < b.path().filename().string();
    });
    return entries;
}

// Function to format time
void format_time(double sec, char* buf, size_t bufsize) {
    long t = static_cast<long>(sec);
    long mm = t / 60;
    long ss = t % 60;
    snprintf(buf, bufsize, "%02ld:%02ld", mm, ss);
}

// Function to play a single file
bool play_file(const std::string& path) {
    freopen("/dev/null", "w", stderr);
    if (mpg123_init() != MPG123_OK) return false;
    mpg123_handle* mh = mpg123_new(nullptr, nullptr);

    if (!mh) {
        mpg123_exit();
        return false;
    }

    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    long rate;
    int channels;
    int encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);
    off_t length = mpg123_length(mh);
    double totalSec = 0.0;

    if (length > 0) {
        totalSec = static_cast<double>(length) / static_cast<double>(rate);
    }

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
    unsigned char buffer[BUFFER_SIZE];
    bool wasPaused = false;
    fftw_plan fftPlan;
    double* fftIn = (double*)fftw_malloc(sizeof(double) * FFT_SIZE);
    fftw_complex* fftOut = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * (FFT_SIZE / 2 + 1));
    fftPlan = fftw_plan_dft_r2c_1d(FFT_SIZE, fftIn, fftOut, FFTW_MEASURE);
    {
        std::lock_guard<std::mutex> lock(playbackMutex);
        playbackStatus.currentTrack = path;
        playbackStatus.currentSec = 0.0;
        playbackStatus.totalSec = totalSec;
        playbackStatus.waveform.assign(FRAMES_PER_BUFFER, 0);
        playbackStatus.spectrum.assign(FFT_SIZE / 2, 0.0);
    }

    while (!shouldQuit.load() && !stopTrack.load()) {
        if (needResize.load()) {
            needResize.store(false);
        }

        if (isPaused.load()) {
            if (!wasPaused) {
                Pa_StopStream(stream);
                wasPaused = true;
            }

            std::unique_lock<std::mutex> lock(pauseMutex);
            pauseCV.wait(lock, [] { return !isPaused.load() || shouldQuit.load() || stopTrack.load(); });
            if (!isPaused.load() && Pa_IsStreamStopped(stream) == 1) {
                Pa_StartStream(stream);
            }
            wasPaused = false;
            if (shouldQuit.load() || stopTrack.load()) {
                break;
            }
        }

        int cmd = seekCommand.exchange(0);
        if (cmd != 0) {
            off_t curPos = mpg123_tell(mh);
            if (curPos < 0) curPos = 0;
            off_t newPos = curPos + cmd * rate;
            if (newPos < 0) newPos = 0;
            if (length > 0 && newPos > length) newPos = length;
            mpg123_seek(mh, newPos, SEEK_SET);
        }

        size_t done = 0;
        int ret = mpg123_read(mh, buffer, BUFFER_SIZE, &done);
        if (ret == MPG123_DONE) break;
        if (ret != MPG123_OK) break;
        int frames = done / (channels * sizeof(short));
        if (Pa_WriteStream(stream, buffer, frames) != paNoError) break;
        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            playbackStatus.currentSec = static_cast<double>(mpg123_tell(mh)) / static_cast<double>(rate);
            int16_t* samples = reinterpret_cast<int16_t*>(buffer);
            int totalSamplesFrame = done / sizeof(int16_t) / channels;
            playbackStatus.waveform.assign(samples, samples + totalSamplesFrame);
        }

        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            if (playbackStatus.waveform.size() >= FFT_SIZE) {
                for (int i = 0; i < FFT_SIZE; i++) {
                    fftIn[i] = static_cast<double>(playbackStatus.waveform[i]) / 32768.0;
                }
                for (int i = FFT_SIZE; i < FRAMES_PER_BUFFER; i++) {
                    fftIn[i] = 0.0;
                }
                fftw_execute(fftPlan);
                double maxMag = 0.0;
                for (int i = 0; i < FFT_SIZE / 2; i++) {
                    double mag = sqrt(fftOut[i][0] * fftOut[i][0] + fftOut[i][1] * fftOut[i][1]);
                    playbackStatus.spectrum[i] = mag;
                    if (mag > maxMag) {
                        maxMag = mag;
                    }
                }
                for (auto& mag : playbackStatus.spectrum) {
                    mag = mag / maxMag;
                }
            }
        }

        usleep(10000);
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
    return true;
}

// Audio playback thread
void audio_thread_func() {
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

// Function to draw the navigation window with scrolling
void draw_navigation(const fs::path& current, const std::vector<fs::directory_entry>& dirList, int highlight) {
    werase(navWin);
    box(navWin, 0, 0);
    wattron(navWin, COLOR_PAIR(1));
    mvwprintw(navWin, 0, 2, " File Browser ");
    wattroff(navWin, COLOR_PAIR(1));
    mvwprintw(navWin, 1, 2, "[..]");

    int navH, navW;
    getmaxyx(navWin, navH, navW);
    int visibleLines = navH - HEADER_LINES - HELP_LINES;

    if (highlight >= listOffset + visibleLines) {
        listOffset = highlight - visibleLines + 1;
    }

    if (highlight < listOffset) {
        listOffset = highlight;
    }

    if (listOffset < 0) listOffset = 0;

    if (listOffset > static_cast<int>(dirList.size()) - visibleLines) {
        listOffset = static_cast<int>(dirList.size()) - visibleLines;
    }

    if (listOffset < 0) listOffset = 0;

    for (int i = 0; i < visibleLines; i++) {
        int idx = listOffset + i;
        if (idx >= static_cast<int>(dirList.size())) break;
        if (idx + 1 == highlight) wattron(navWin, A_REVERSE);
        std::string nm = dirList[idx].path().filename().string();
        if (dirList[idx].is_directory()) nm += "/";
        if (nm.length() > static_cast<size_t>(navW - 4)) {
            nm = nm.substr(0, navW - 7) + "...";
        }
        mvwprintw(navWin, i + HEADER_LINES, 2, "%s", nm.c_str());
        if (idx + 1 == highlight) wattroff(navWin, A_REVERSE);
    }

    wattron(navWin, COLOR_PAIR(3));
    mvwprintw(navWin, navH - HELP_LINES, 2, "[1] Waveform  [2] Spectrum  [LEFT/RIGHT] seek 5s  [p] pause  [r] resume");
    mvwprintw(navWin, navH - HELP_LINES + 1, 2, "[ENTER] open/add mp3  [a|l] add all mp3");
    mvwprintw(navWin, navH - HELP_LINES + 2, 2, "[s] skip  [x] stop  [q] quit  Current: %s", current.c_str());
    wattroff(navWin, COLOR_PAIR(3));
    wrefresh(navWin);
}

// Function to draw the waveform visualization
void draw_waveform(const int waveH, const int waveW) {
    std::lock_guard<std::mutex> lock(playbackMutex);
    werase(waveWin);
    box(waveWin, 0, 0);
    wattron(waveWin, COLOR_PAIR(1));
    mvwprintw(waveWin, 0, 2, "Playing: %s", playbackStatus.currentTrack.c_str());
    wattroff(waveWin, COLOR_PAIR(1));

    char cb[16];
    char tb[16];
    format_time(playbackStatus.currentSec, cb, sizeof(cb));
    format_time(playbackStatus.totalSec, tb, sizeof(tb));
    wattron(waveWin, COLOR_PAIR(3));
    mvwprintw(waveWin, waveH - 1, 2, "%s / %s", cb, tb);
    mvwprintw(waveWin, waveH - 1, 20, "Mode: %s", (visMode.load() == WAVEFORM) ? "Waveform" : "Spectrum");
    wattroff(waveWin, COLOR_PAIR(3));

    if (visMode.load() == WAVEFORM) {
        const auto& waveform = playbackStatus.waveform;
        if (waveform.empty()) return;

        for (int i = 0; i < waveW; i++) {
            int idx = (i * waveform.size()) / waveW;
            if (idx >= static_cast<int>(waveform.size())) idx = waveform.size() - 1;
            double val = static_cast<double>(waveform[idx]) / 32768.0;
            int y = static_cast<int>((val + 1.0) * 0.5 * (waveH - 3));
            if (y < 0) y = 0;
            if (y >= waveH - 2) y = waveH - 3;
            wattron(waveWin, COLOR_PAIR(2));
            mvwprintw(waveWin, (waveH - 2) - y - 1, i, "*");
            wattroff(waveWin, COLOR_PAIR(2));
        }
    } else if (visMode.load() == SPECTRUM) {
        const auto& spectrum = playbackStatus.spectrum;
        if (spectrum.empty()) return;

        int numBars = waveW;
        int binsPerBar = (FFT_SIZE / 2) / numBars;
        if (binsPerBar < 1) binsPerBar = 1;
        std::vector<double> barMagnitudes(numBars, 0.0);
        for (int i = 0; i < numBars; i++) {
            int startIdx = i * binsPerBar;
            int endIdx = startIdx + binsPerBar;
            if (endIdx > FFT_SIZE / 2) endIdx = FFT_SIZE / 2;
            double sum = 0.0;
            for (int j = startIdx; j < endIdx; j++) {
                sum += spectrum[j];
            }

            double avg = sum / (endIdx - startIdx);
            barMagnitudes[i] = avg;
        }

        for (int i = 0; i < numBars; i++) {
            double mag = barMagnitudes[i];
            int barHeight = static_cast<int>(mag * (waveH - 3));
            if (barHeight > waveH - 3) barHeight = waveH - 3;
            for (int y = 0; y < barHeight; y++) {
                wattron(waveWin, COLOR_PAIR(2));
                mvwprintw(waveWin, waveH - 2 - y, i, "|");
                wattroff(waveWin, COLOR_PAIR(2));
            }
        }
    }

    wrefresh(waveWin);
}

// Signal handler for window resize
void on_resize(int) {
    needResize.store(true);
}

int main() {
    struct sigaction sa;
    sa.sa_handler = on_resize;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &sa, nullptr) == -1) {
        std::cerr << "Failed to set up SIGWINCH handler." << std::endl;
        return 1;
    }

    init_tui();
    fs::path currentDir = fs::path(getenv("HOME"));
    auto dirList = list_directory(currentDir);
    int highlight = 0;
    std::thread audioThread(audio_thread_func);
    bool redraw = true;

    while (!shouldQuit.load()) {
        if (needResize.load()) {
            needResize.store(false);
            cleanup_and_reinit_windows();
            redraw = true;
        }

        {
            int waveH, waveW;
            getmaxyx(waveWin, waveH, waveW);
            draw_waveform(waveH, waveW);
        }

        if (redraw) {
            draw_navigation(currentDir, dirList, highlight);
            redraw = false;
        }

        int c = getch();
        if (c == 'q' || c == 'Q') {
            shouldQuit.store(true);
            playlistCV.notify_all();
            pauseCV.notify_one();
            break;
        } else if (c == KEY_UP) {
            if (highlight > 0) {
                highlight--;
                redraw = true;
            }
        } else if (c == KEY_DOWN) {
            if (highlight < static_cast<int>(dirList.size())) {
                highlight++;
                redraw = true;
            }
        } else if (c == KEY_LEFT) {
            seekCommand.store(-5);
        } else if (c == KEY_RIGHT) {
            seekCommand.store(5);
        } else if (c == 'p' || c == 'P') {
            isPaused.store(true);
        } else if (c == 'r' || c == 'R') {
            isPaused.store(false);
            pauseCV.notify_one();
        } else if (c == '1') {
            visMode.store(WAVEFORM);
        } else if (c == '2') {
            visMode.store(SPECTRUM);
        } else if (c == '\n') {
            if (highlight == 0) {
                if (currentDir.has_parent_path()) {
                    currentDir = currentDir.parent_path();
                    dirList = list_directory(currentDir);
                    highlight = 0;
                }
            } else {
                int idx = highlight - 1 + listOffset;
                if (idx >= 0 && idx < static_cast<int>(dirList.size())) {
                    fs::directory_entry sel = dirList[idx];
                    if (sel.is_directory()) {
                        currentDir = sel.path();
                        dirList = list_directory(currentDir);
                        highlight = 0;
                    } else {
                        std::string ext = sel.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".mp3") {
                            {
                                std::lock_guard<std::mutex> lk(playlistMutex);
                                playlist.push_back(sel.path().string());
                            }
                            playlistCV.notify_one();
                        }
                    }
                }
            }
            redraw = true;
        } else if (c == 'a' || c == 'A' || c == 'l' || c == 'L') {
            {
                std::lock_guard<std::mutex> lk(playlistMutex);
                for (auto& e : dirList) {
                    if (!e.is_directory()) {
                        std::string ex = e.path().extension().string();
                        std::transform(ex.begin(), ex.end(), ex.begin(), ::tolower);
                        if (ex == ".mp3") {
                            playlist.push_back(e.path().string());
                        }
                    }
                }
            }
            playlistCV.notify_one();
            redraw = true;
        } else if (c == 's' || c == 'S') {
            stopTrack.store(true);
        } else if (c == 'x' || c == 'X') {
            {
                std::lock_guard<std::mutex> lk(playlistMutex);
                playlist.clear();
            }
            stopTrack.store(true);
        }
        usleep(30000);
    }

    if (audioThread.joinable()) audioThread.join();
    close_tui();
    return 0;
}
