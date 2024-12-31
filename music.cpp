#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <cstdio> // For freopen

// ncurses for TUI
#include <ncurses.h>

// libmpg123 for MP3 decoding
#include <mpg123.h>

// PortAudio for audio playback
#include <portaudio.h>

// Include cmath for sqrt and other math functions
#include <cmath>

// Use std::filesystem for directory operations
namespace fs = std::filesystem;

// Constants
#define BUFFER_SIZE 8192
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define FRAMES_PER_BUFFER 512

// Global flags
std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<bool> shouldQuit(false);

// Mutex and condition variable for pause/resume
std::mutex mtx;
std::condition_variable cv;

// Function to initialize ncurses
void init_ncurses_ui() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);
    if (has_colors() == FALSE) {
        endwin();
        std::cerr << "Your terminal does not support color\n";
        exit(1);
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
}

// Function to terminate ncurses
void terminate_ncurses_ui() {
    endwin();
}

// Function to list directory contents
std::vector<fs::directory_entry> list_directory(const fs::path& path) {
    std::vector<fs::directory_entry> entries;
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            entries.push_back(entry);
        }
    } catch (fs::filesystem_error& e) {
        // Handle permission errors or other issues
    }
    // Sort directories first, then files
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        if (a.is_directory() && !b.is_directory()) return true;
        if (!a.is_directory() && b.is_directory()) return false;
        return a.path().filename().string() < b.path().filename().string();
    });
    return entries;
}

// Audio playback and waveform visualization
bool play_audio(const std::string& filepath) {
    // Redirect stderr to suppress ALSA warnings
    freopen("/dev/null", "w", stderr);

    // Initialize mpg123
    if (mpg123_init() != MPG123_OK) {
        std::cerr << "Failed to initialize mpg123.\n";
        return false;
    }

    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) {
        std::cerr << "Failed to create mpg123 handle.\n";
        mpg123_exit();
        return false;
    }

    if (mpg123_open(mh, filepath.c_str()) != MPG123_OK) {
        std::cerr << "Failed to open MP3 file.\n";
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    // Get audio format
    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        std::cerr << "Failed to get MP3 format.\n";
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    // Ensure that output format does not change
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);

    // Initialize PortAudio
    PaError paErr = Pa_Initialize();
    if (paErr != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(paErr) << "\n";
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    // Open PortAudio stream
    PaStream *stream;
    PaStreamParameters outputParameters;
    outputParameters.device = Pa_GetDefaultOutputDevice(); // Default device
    if (outputParameters.device == paNoDevice) {
        std::cerr << "No default output device.\n";
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }
    outputParameters.channelCount = channels;
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    paErr = Pa_OpenStream(&stream,
                          NULL, // no input
                          &outputParameters,
                          rate,
                          FRAMES_PER_BUFFER,
                          paClipOff,
                          NULL,
                          NULL);
    if (paErr != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(paErr) << "\n";
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    paErr = Pa_StartStream(stream);
    if (paErr != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(paErr) << "\n";
        Pa_CloseStream(stream);
        Pa_Terminate();
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return false;
    }

    // Buffer for audio data
    unsigned char buffer[BUFFER_SIZE];
    size_t done = 0;

    // Waveform visualization variables
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    std::vector<int16_t> waveform(max_x, 0); // Store waveform data

    while (!shouldQuit.load()) {
        // Handle pause
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [] { return !isPaused.load() || shouldQuit.load(); });
            if (shouldQuit.load()) break;
        }

        int ret = mpg123_read(mh, buffer, BUFFER_SIZE, &done);
        if (ret == MPG123_DONE) {
            break; // End of file
        } else if (ret != MPG123_OK) {
            std::cerr << "mpg123 read error: " << mpg123_strerror(mh) << "\n";
            break;
        }

        // Write audio data to PortAudio
        paErr = Pa_WriteStream(stream, buffer, done / (channels * sizeof(short)));
        if (paErr != paNoError) {
            std::cerr << "PortAudio write error: " << Pa_GetErrorText(paErr) << "\n";
            break;
        }

        // Prepare data for waveform
        int16_t* samples = (int16_t*)buffer;
        int num_samples = done / sizeof(int16_t) / channels;

        // Simple downsampling to fit the terminal width
        for (int i = 0; i < max_x; ++i) {
            int sample_index = (i * num_samples) / max_x;
            if (sample_index < num_samples) {
                waveform[i] = samples[sample_index * channels]; // Mono channel
            } else {
                waveform[i] = 0;
            }
        }

        // Display waveform
        clear();
        attron(COLOR_PAIR(1));
        mvprintw(0, 0, "Playing: %s", filepath.c_str());
        attroff(COLOR_PAIR(1));
        for (int i = 0; i < max_x; ++i) {
            // Normalize sample to terminal height
            double normalized = (double)waveform[i] / 32768.0;
            int y = (int)((normalized + 1.0) / 2.0 * (max_y - 1)); // Map from [-1,1] to [0, max_y-1]
            if (y < 0) y = 0;
            if (y >= max_y) y = max_y - 1;
            attron(COLOR_PAIR(2));
            mvprintw(max_y - y - 1, i, "*");
            attroff(COLOR_PAIR(2));
        }
        refresh();
    }

    // Cleanup
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return true;
}

int main() {
    // Initialize ncurses
    init_ncurses_ui();

    // Set starting directory to home
    fs::path current_path = fs::path(getenv("HOME"));

    // List entries
    std::vector<fs::directory_entry> entries = list_directory(current_path);

    int highlight = 0;
    int c;

    while (true) {
        clear();
        mvprintw(0, 0, "Current Directory: %s", current_path.c_str());
        for (size_t i = 0; i < entries.size(); ++i) {
            if ((int)i == highlight) {
                attron(A_REVERSE);
            }
            std::string name = entries[i].path().filename().string();
            if (entries[i].is_directory()) {
                name += "/";
            }
            mvprintw(2 + i, 2, "%s", name.c_str());
            if ((int)i == highlight) {
                attroff(A_REVERSE);
            }
        }
        refresh();

        c = getch();
        if (c == KEY_UP) {
            highlight = (highlight - 1 + entries.size()) % entries.size();
        } else if (c == KEY_DOWN) {
            highlight = (highlight + 1) % entries.size();
        } else if (c == '\n' || c == KEY_ENTER) {
            if (entries.empty()) continue;
            fs::directory_entry selected = entries[highlight];
            if (selected.is_directory()) {
                current_path = selected.path();
                entries = list_directory(current_path);
                highlight = 0;
            } else {
                std::string ext = selected.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".mp3") {
                    // Play the MP3 file
                    terminate_ncurses_ui();
                    isPlaying = true;
                    isPaused = false;
                    shouldQuit = false;

                    std::thread audioThread([&]() {
                        play_audio(selected.path().string());
                        isPlaying = false;
                    });

                    // Handle user input during playback
                    while (isPlaying.load()) {
                        int key = getch();
                        if (key == 'p') {
                            isPaused = true;
                        } else if (key == 'r') {
                            if (isPaused.load()) {
                                isPaused = false;
                                cv.notify_one();
                            }
                        } else if (key == 'q') {
                            shouldQuit = true;
                            cv.notify_one();
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    audioThread.join();
                    // Reinitialize ncurses after playback
                    init_ncurses_ui();
                }
            }
        } else if (c == 'q' || c == 'Q') {
            break;
        }
    }

    // Terminate ncurses
    terminate_ncurses_ui();

    return 0;
}
