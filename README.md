# TerminalWave

TerminalWave is a terminal-based music player with a visual waveform display. It combines simplicity with functionality, enabling users to browse directories, play MP3 files and enjoy real time waveform visualisations, all within the terminal.

## Features

- **Intuitive Terminal User Interface**: Navigate directories with ease using arrow keys.
- **MP3 Playback**: Play MP3 files directly from your terminal.
- **Real-Time Waveform Visualisation**: Enjoy dynamic waveforms rendered in your terminal.
- **Cross-Platform Support**: Works seamlessly on Linux and Termux (Android).
- **Pause, Resume, and Quit Controls**:
  - `p`: Pause playback
  - `r`: Resume playback
  - `q`: Quit playback

## Requirements

Make sure the following dependencies are installed on your system:

- **C++ Compiler** (e.g. `g++` or `clang++`)
- **ncurses**: For terminal UI.
- **mpg123**: For MP3 decoding.
- **PortAudio**: For audio playback.

### Installing Dependencies

#### Arch Linux
```bash
sudo pacman -S base-devel ncurses mpg123 portaudio
```

#### Ubuntu/Linux Mint
```bash
sudo apt install build-essential libncurses5-dev libmpg123-dev libportaudio2
```

#### Termux
```bash
pkg install clang ncurses mpg123 portaudio
```

## Building and Running TerminalWave
```bash
git clone https://github.com/Techlm77/TerminalWave.git
cd TerminalWave
clang++ music.cpp -o music -lncurses -lmpg123 -lportaudio -lpthread -lm -lfftw3
./music
```

## Enjoy!!
