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
#include <fstream>
#include <fftw3.h>
#include <chrono>

#if __cplusplus < 201703L
  #define clamp(val, lo, hi) (((val) < (lo)) ? (lo) : (((val) > (hi)) ? (hi) : (val)))
#else
  using std::clamp;
#endif

#define BUFFER_SIZE 8192
#define FRAMES_PER_BUFFER 512
#define FFT_SIZE 1024

enum VisualizationMode {
    WAVEFORM = 1,
    SPECTRUM = 2
};

void close_tui();
void handle_resize();
void init_tui();
std::vector<fs::directory_entry> list_directory(const fs::path& p);
void format_time(double sec, char* buf, size_t bufsize);
void draw_info(const std::string& filepath, double currentSec, double totalSec, VisualizationMode mode);
bool play_file(const std::string& path);
void audio_thread();
void draw_navigation(const fs::path& current, const std::vector<fs::directory_entry>& dirList, int highlight);
void on_resize(int);
void handle_sigint(int);

std::atomic<bool> shouldQuit(false);
std::atomic<bool> stopTrack(false);
std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<int> seekCommand(0);
std::atomic<VisualizationMode> visMode(WAVEFORM);

std::mutex playlistMutex;
std::deque<std::string> playlist;
std::condition_variable playlistCV;

std::atomic<bool> needResize(false);

std::mutex pauseMutex;
std::condition_variable pauseCV;

WINDOW* navWin = nullptr;
WINDOW* infoWin = nullptr;
WINDOW* waveWin = nullptr;

int totalH = 0;
int totalW = 0;
int halfH = 0;
int halfW = 0;
int listOffset = 0;

std::mutex uiMutex;

void close_tui() {
    if (waveWin) { delwin(waveWin); waveWin = nullptr; }
    if (infoWin) { delwin(infoWin); infoWin = nullptr; }
    if (navWin) { delwin(navWin); navWin = nullptr; }
    endwin();
}

void handle_resize() {
    std::lock_guard<std::mutex> lock(uiMutex);
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return;
    }
    totalH = ws.ws_row;
    totalW = ws.ws_col;
    if (totalH < 10 || totalW < 20) return;
    halfH = totalH / 2;
    halfW = totalW / 2;
    
    resizeterm(totalH, totalW);
    clear();
    refresh();
    
    if (navWin) { delwin(navWin); navWin = nullptr; }
    if (infoWin) { delwin(infoWin); infoWin = nullptr; }
    if (waveWin) { delwin(waveWin); waveWin = nullptr; }
    
    navWin = newwin(halfH, halfW, 0, 0);
    box(navWin, 0, 0);
    
    infoWin = newwin(halfH, totalW - halfW, 0, halfW);
    box(infoWin, 0, 0);
    
    waveWin = newwin(totalH - halfH, totalW, halfH, 0);
    box(waveWin, 0, 0);
    
    wrefresh(navWin);
    wrefresh(infoWin);
    wrefresh(waveWin);
}

void init_tui() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, true);
    
    if (!has_colors()) {
        endwin();
        std::cerr << "Error: Terminal does not support colors." << std::endl;
        exit(1);
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    
    mousemask(0, nullptr);
    
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        getmaxyx(stdscr, totalH, totalW);
    } else {
        totalH = ws.ws_row;
        totalW = ws.ws_col;
    }
    if (totalH < 10 || totalW < 20) {
        endwin();
        std::cerr << "Error: Terminal window too small." << std::endl;
        exit(1);
    }
    halfH = totalH / 2;
    halfW = totalW / 2;
    
    navWin = newwin(halfH, halfW, 0, 0);
    box(navWin, 0, 0);
    
    infoWin = newwin(halfH, totalW - halfW, 0, halfW);
    box(infoWin, 0, 0);
    
    waveWin = newwin(totalH - halfH, totalW, halfH, 0);
    box(waveWin, 0, 0);
    
    clear();
    refresh();
    wrefresh(navWin);
    wrefresh(infoWin);
    wrefresh(waveWin);
}

std::vector<fs::directory_entry> list_directory(const fs::path& p) {
    std::vector<fs::directory_entry> entries;
    try {
        for (auto& x : fs::directory_iterator(p))
            entries.push_back(x);
    } catch (const std::exception& e) {
        std::cerr << "Error accessing directory: " << e.what() << std::endl;
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        if (a.is_directory() && !b.is_directory()) return true;
        if (!a.is_directory() && b.is_directory()) return false;
        return a.path().filename().string() < b.path().filename().string();
    });
    return entries;
}

void format_time(double sec, char* buf, size_t bufsize) {
    long t = static_cast<long>(sec);
    long mm = t / 60, ss = t % 60;
    snprintf(buf, bufsize, "%02ld:%02ld", mm, ss);
}

void draw_info(const std::string& filepath, double currentSec, double totalSec, VisualizationMode mode) {
    if (!infoWin) return;
    std::lock_guard<std::mutex> lock(uiMutex);
    werase(infoWin);
    box(infoWin, 0, 0);
    
    fs::path p(filepath);
    std::string filename = p.filename().string();
    
    wattron(infoWin, COLOR_PAIR(1));
    mvwprintw(infoWin, 1, 2, "Now Playing: %s", filename.c_str());
    wattroff(infoWin, COLOR_PAIR(1));
    
    mvwprintw(infoWin, 3, 2, "Progress:");
    int max_x, max_y;
    getmaxyx(infoWin, max_y, max_x);
    int barWidth = (max_x - 4 < 10) ? 10 : max_x - 4;
    int barY = 4;
    double progress = (totalSec > 0.0) ? (currentSec / totalSec) : 0.0;
    if (progress > 1.0) progress = 1.0;
    int filled = static_cast<int>(progress * (barWidth - 2));
    
    wattron(infoWin, COLOR_PAIR(2));
    mvwprintw(infoWin, barY, 2, "[");
    for (int i = 0; i < barWidth - 2; ++i)
        waddch(infoWin, (i < filled) ? '#' : '-');
    waddch(infoWin, ']');
    wattroff(infoWin, COLOR_PAIR(2));
    
    char cb[16], tb[16];
    format_time(currentSec, cb, sizeof(cb));
    format_time(totalSec, tb, sizeof(tb));
    mvwprintw(infoWin, barY + 1, 2, "%s / %s", cb, tb);
    
    wattron(infoWin, COLOR_PAIR(3));
    mvwprintw(infoWin, barY + 3, 2, "Visualization: %s", (mode == WAVEFORM) ? "Waveform" : "Spectrum");
    wattroff(infoWin, COLOR_PAIR(3));
    wrefresh(infoWin);
}

bool play_file(const std::string& path) {
    freopen("/dev/null", "w", stderr);
    
    if (mpg123_init() != MPG123_OK) {
        std::cerr << "mpg123 initialization failed." << std::endl;
        return false;
    }
    mpg123_handle* mh = mpg123_new(nullptr, nullptr);
    if (!mh) {
        std::cerr << "Failed to create mpg123 handle." << std::endl;
        mpg123_exit();
        return false;
    }
    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        std::cerr << "Failed to open MP3 file: " << path << std::endl;
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }
    
    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        std::cerr << "Failed to get MP3 format." << std::endl;
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        return false;
    }
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);
    
    off_t length = mpg123_length(mh);
    double totalSec = (length > 0) ? double(length) / double(rate) : 0.0;
    
    if (Pa_Initialize() != paNoError) {
        std::cerr << "PortAudio initialization failed." << std::endl;
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        return false;
    }
    PaStreamParameters out;
    out.device = Pa_GetDefaultOutputDevice();
    if (out.device == paNoDevice) {
        std::cerr << "No default audio output device." << std::endl;
        Pa_Terminate();
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        return false;
    }
    out.channelCount = channels;
    out.sampleFormat = paInt16;
    out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = nullptr;
    
    PaStream* stream = nullptr;
    if (Pa_OpenStream(&stream, nullptr, &out, rate, FRAMES_PER_BUFFER, paClipOff, nullptr, nullptr) != paNoError) {
        std::cerr << "Failed to open PortAudio stream." << std::endl;
        Pa_Terminate();
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        return false;
    }
    if (Pa_StartStream(stream) != paNoError) {
        std::cerr << "Failed to start PortAudio stream." << std::endl;
        Pa_CloseStream(stream); Pa_Terminate();
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
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
    if (!fftIn || !fftOut) {
        std::cerr << "FFTW memory allocation failed." << std::endl;
        if (fftIn) fftw_free(fftIn);
        if (fftOut) fftw_free(fftOut);
        Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate();
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        isPlaying.store(false);
        return false;
    }
    fftPlan = fftw_plan_dft_r2c_1d(FFT_SIZE, fftIn, fftOut, FFTW_MEASURE);
    if (!fftPlan) {
        std::cerr << "FFTW plan creation failed." << std::endl;
        fftw_free(fftIn); fftw_free(fftOut);
        Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate();
        mpg123_close(mh); mpg123_delete(mh); mpg123_exit();
        isPlaying.store(false);
        return false;
    }
    
    std::string currentFile = path;
    double currentSec = 0.0;
    
    while (!shouldQuit.load() && !stopTrack.load()) {
        if (needResize.load()) {
            needResize.store(false);
            handle_resize();
        }
        if (isPaused.load()) {
            if (!wasPaused) { Pa_StopStream(stream); wasPaused = true; }
            std::unique_lock<std::mutex> lock(pauseMutex);
            pauseCV.wait(lock, [] { return !isPaused.load() || shouldQuit.load() || stopTrack.load(); });
            if (!isPaused.load() && Pa_IsStreamStopped(stream) == 1)
                Pa_StartStream(stream);
            wasPaused = false;
            if (shouldQuit.load() || stopTrack.load()) break;
        }
        int cmd = seekCommand.exchange(0);
        if (cmd != 0) {
            off_t curPos = mpg123_tell(mh);
            if (curPos < 0) curPos = 0;
            off_t newPos = curPos + cmd * rate;
            if (newPos < 0) newPos = 0;
            if (length > 0 && newPos > length) newPos = length;
            if (mpg123_seek(mh, newPos, SEEK_SET) < 0)
                std::cerr << "Seek operation failed." << std::endl;
        }
        
        if (!waveWin || !infoWin) break;
        int waveH, waveW;
        getmaxyx(waveWin, waveH, waveW);
        if (waveH < 4 || waveW < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        
        size_t done = 0;
        int ret = mpg123_read(mh, buffer, BUFFER_SIZE, &done);
        if (ret == MPG123_DONE || ret != MPG123_OK)
            break;
        
        int frames = done / (channels * sizeof(short));
        if (Pa_WriteStream(stream, buffer, frames) != paNoError)
            break;
        
        int16_t* samples = reinterpret_cast<int16_t*>(buffer);
        int totalSamplesFrame = done / sizeof(int16_t) / channels;
        
        off_t curSamp = mpg123_tell(mh);
        if (curSamp >= 0)
            currentSec = double(curSamp) / double(rate);
        
        draw_info(currentFile, currentSec, totalSec, visMode.load());
        
        if (visMode.load() == WAVEFORM) {
            std::vector<int16_t> waveform(waveW, 0);
            for (int i = 0; i < waveW; i++) {
                int idx = (i * totalSamplesFrame) / waveW;
                if (idx < totalSamplesFrame)
                    waveform[i] = samples[idx * channels];
            }
            werase(waveWin);
            box(waveWin, 0, 0);
            for (int i = 0; i < waveW; i++) {
                double normalizedValue = double(waveform[i]) / 32768.0;
                int y = int((normalizedValue + 1.0) * 0.5 * (waveH - 3));
                y = clamp(y, 0, waveH - 3);
                wattron(waveWin, COLOR_PAIR(2));
                mvwaddch(waveWin, (waveH - 2) - y, i, '#');
                wattroff(waveWin, COLOR_PAIR(2));
            }
        } else {
            for (size_t i = 0; i < size_t(totalSamplesFrame) && i < FFT_SIZE; i++)
                fftIn[i] = double(samples[i * channels]) / 32768.0;
            for (size_t i = totalSamplesFrame; i < FFT_SIZE; i++)
                fftIn[i] = 0.0;
            fftw_execute(fftPlan);
            
            std::vector<double> magnitudes(FFT_SIZE / 2, 0.0);
            double maxMag = 1e-9;
            for (size_t i = 0; i < FFT_SIZE / 2; i++) {
                double re = fftOut[i][0], im = fftOut[i][1];
                double magVal = sqrt(re * re + im * im);
                magnitudes[i] = magVal;
                if (magVal > maxMag) maxMag = magVal;
            }
            
            int numBars = waveW;
            int binsPerBar = (FFT_SIZE / 2) / numBars;
            if (binsPerBar < 1) binsPerBar = 1;
            std::vector<double> barMagnitudes(numBars, 0.0);
            for (int i = 0; i < numBars; i++) {
                int startIdx = i * binsPerBar;
                int endIdx = std::min(startIdx + binsPerBar, (int)FFT_SIZE / 2);
                double sum = 0.0;
                for (int j = startIdx; j < endIdx; j++)
                    sum += magnitudes[j];
                barMagnitudes[i] = sum / (endIdx - startIdx);
            }
            
            werase(waveWin);
            box(waveWin, 0, 0);
            for (int i = 0; i < numBars; i++) {
                double ratio = log(barMagnitudes[i] + 1.0) / log(maxMag + 1.0);
                int barHeight = int(ratio * (waveH - 3));
                barHeight = clamp(barHeight, 0, waveH - 3);
                for (int y = 0; y < barHeight; y++) {
                    wattron(waveWin, COLOR_PAIR(2));
                    mvwaddch(waveWin, waveH - 2 - y, i, '|');
                    wattroff(waveWin, COLOR_PAIR(2));
                }
            }
        }
        wrefresh(waveWin);
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

void audio_thread() {
    while (!shouldQuit.load()) {
        std::string nextPath;
        {
            std::unique_lock<std::mutex> lock(playlistMutex);
            playlistCV.wait(lock, [] { return shouldQuit.load() || !playlist.empty(); });
            if (shouldQuit.load()) break;
            if (!playlist.empty()) {
                nextPath = playlist.front();
                playlist.pop_front();
            } else continue;
        }
        stopTrack.store(false);
        play_file(nextPath);
        if (shouldQuit.load()) break;
    }
    isPlaying.store(false);
}

void draw_navigation(const fs::path& current, const std::vector<fs::directory_entry>& dirList, int highlight) {
    if (!navWin) return;
    std::lock_guard<std::mutex> lock(uiMutex);
    werase(navWin);
    box(navWin, 0, 0);
    
    wattron(navWin, COLOR_PAIR(1));
    mvwprintw(navWin, 0, 2, " File Browser ");
    wattroff(navWin, COLOR_PAIR(1));
    
    int navH, navW;
    getmaxyx(navWin, navH, navW);
    int linesForItems = (navH - 2) - 3;
    if (linesForItems < 1) { wrefresh(navWin); return; }
    int totalItems = static_cast<int>(dirList.size()) + 1;
    if (highlight < 0) highlight = 0;
    if (highlight >= totalItems) highlight = totalItems - 1;
    if (highlight < listOffset) listOffset = highlight;
    if (highlight >= listOffset + linesForItems) listOffset = highlight - linesForItems + 1;
    if (listOffset < 0) listOffset = 0;
    if (listOffset >= totalItems) listOffset = totalItems - 1;
    
    for (int i = 0; i < linesForItems; i++) {
        int idx = listOffset + i;
        if (idx >= totalItems) break;
        int screenY = i + 1;
        if (idx == 0) {
            if (highlight == 0) wattron(navWin, A_REVERSE);
            mvwprintw(navWin, screenY, 2, "[..]");
            if (highlight == 0) wattroff(navWin, A_REVERSE);
        } else {
            int realIndex = idx - 1;
            if (realIndex < 0 || realIndex >= static_cast<int>(dirList.size())) break;
            if (highlight == idx) wattron(navWin, A_REVERSE);
            std::string nm = dirList[realIndex].path().filename().string();
            if (dirList[realIndex].is_directory()) nm += "/";
            if (static_cast<int>(nm.size()) > navW - 4)
                nm = nm.substr(0, navW - 7) + "...";
            mvwprintw(navWin, screenY, 2, "%s", nm.c_str());
            if (highlight == idx) wattroff(navWin, A_REVERSE);
        }
    }
    wattron(navWin, COLOR_PAIR(3));
    mvwprintw(navWin, navH - 3, 2, "[1] Waveform  [2] Spectrum  [LEFT/RIGHT] seek 5s  [p] pause  [r] resume");
    mvwprintw(navWin, navH - 2, 2, "[ENTER] open/add mp3  [a|l] add all mp3");
    mvwprintw(navWin, navH - 1, 2, "[s] skip  [x] stop  [q] quit  Current: %s", current.string().c_str());
    wattroff(navWin, COLOR_PAIR(3));
    wrefresh(navWin);
}

void on_resize(int) { needResize.store(true); }
void handle_sigint(int) { shouldQuit.store(true); playlistCV.notify_all(); pauseCV.notify_all(); }

int main() {
    signal(SIGWINCH, on_resize);
    signal(SIGINT, handle_sigint);
    
    init_tui();
    
    const char* homeEnv = getenv("HOME");
    fs::path currentDir = homeEnv ? fs::path(homeEnv) : fs::current_path();
    auto dirList = list_directory(currentDir);
    
    int highlight = 0;
    bool redraw = true;
    
    std::thread at(audio_thread);
    
    while (!shouldQuit.load()) {
        if (needResize.load()) { 
            needResize.store(false); 
            handle_resize(); 
            redraw = true; 
        }
        if (!navWin || !infoWin || !waveWin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
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
            if (highlight > 0) { highlight--; redraw = true; }
        } else if (c == KEY_DOWN) {
            int totalItems = static_cast<int>(dirList.size()) + 1;
            if (highlight < totalItems - 1) { highlight++; redraw = true; }
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
                    listOffset = 0;
                }
            } else {
                int realIndex = highlight - 1;
                if (realIndex >= 0 && realIndex < static_cast<int>(dirList.size())) {
                    fs::directory_entry sel = dirList[realIndex];
                    if (sel.is_directory()) {
                        currentDir = sel.path();
                        dirList = list_directory(currentDir);
                        highlight = 0;
                        listOffset = 0;
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
                        if (ex == ".mp3")
                            playlist.push_back(e.path().string());
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
        } else if (c == KEY_MOUSE) {
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    
    if (at.joinable()) at.join();
    close_tui();
    return 0;
}
