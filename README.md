# Radio Garden CLI

A simple CLI for [Radio Garden](https://radio.garden/). Made for educational purposes for  learning C++.

It works scraping the main page of Radio Garden and parsing the JSON response. It uses [nlohmann/json](https://github.com/nlohmann/json)
for parsing the JSON response. Once the stations are parsed, it uses [mpv](https://mpv.io/) to play the streams.

## Dependencies

On Arch Linux:

```
pacman -S nlohmann-json mpv
```

## Building

```sh
make
```

## Usage

```sh
./radio --help
```

### Examples

Play stations by location:

```sh
./radio -p "Varna"
```

# Comments

I'm not a C++ developer, so I'm sure there are many things that can be improved. I'm open to suggestions and pull requests.
