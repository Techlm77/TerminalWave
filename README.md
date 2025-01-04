# TerminalWave

TerminalWave is a terminal-based music player with a visual waveform display. It combines simplicity with functionality, enabling users to browse directories, play MP3 files, and enjoy real-time waveform visualizations, all within the terminal.

## Features

- **Intuitive Terminal User Interface**: Navigate directories with ease using arrow keys.
- **MP3 Playback**: Play MP3 files directly from your terminal.
- **Real-Time Waveform Visualization**: Enjoy dynamic waveforms rendered in your terminal.
- **Cross-Platform Support**: Works seamlessly on Linux and Termux (Android).
- **Pause, Resume, and Quit Controls**:
  - `p`: Pause playback
  - `r`: Resume playback
  - `q`: Quit playback

## Requirements

Make sure the following dependencies are installed on your system:

- **C++ Compiler** (e.g., `g++` or `clang++`)
- **ncurses**: For terminal UI.
- **mpg123**: For MP3 decoding.
- **PortAudio**: For audio playback.
- **FFTW**: For waveform visualization.

### Installing Dependencies

#### Arch Linux
```bash
sudo pacman -S base-devel ncurses mpg123 portaudio fftw
```

#### Ubuntu/Linux Mint
```bash
sudo apt install build-essential libncurses5-dev libmpg123-dev libportaudio2 fftw
```

#### Termux
```bash
pkg install clang ncurses mpg123 portaudio fftw
```

## Building TerminalWave
```bash
git clone https://github.com/Techlm77/TerminalWave.git
cd TerminalWave
clang++ music.cpp -o music -lncurses -lmpg123 -lportaudio -lpthread -lm -lfftw3
```

## Installing TerminalWave for Easy Access

After building TerminalWave, you can move the executable to a directory in your system's `PATH` to run it from anywhere.

### Linux
```bash
sudo mv music /usr/local/bin
```

### Termux
```bash
mv music $PREFIX/bin
```

## Running TerminalWave
Once installed, you can run TerminalWave from anywhere by simply typing:
```bash
music
```

## Enjoy!!
