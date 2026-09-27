#include "subtitles.h"
#include "process.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace mp42rhm {
  uint32_t subtitle_band_height(const Options& options) {
    return options.subtitle_track ? std::max(32U, options.height / 5) : 0;
  }

  std::wstring prepare_subtitles(const Options& options, const std::filesystem::path& directory) {
    if (!options.subtitle_track)
      return {};
    const auto source = directory / L"source.ass", band = directory / L"band.ass";
    Process extract({options.ffmpeg, L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-i", std::filesystem::absolute(options.input).wstring(), L"-map",
      L"0:s:" + std::to_wstring(options.subtitle_track - 1), L"-c:s", L"ass", source.wstring()});

    extract.finish();
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(band, std::ios::binary);
    const auto height = subtitle_band_height(options);
    std::string line;

    input.exceptions(std::ios::badbit);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << options.width
      << "\nPlayResY: " << options.height << "\nWrapStyle: 0\n\n[V4+ Styles]\n"
      << "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
      << "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
      << "Alignment,MarginL,MarginR,MarginV,Encoding\n"
      << "Style: Band,Arial," << height / 4 << ",&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
      << "-1,0,0,0,100,100,0,0,1,0,0,2," << options.width / 40 << ',' << options.width / 40
      << ',' << height / 12 << ",1\n\n[Events]\n"
      << "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
    while (std::getline(input, line)) {
      if (line.compare(0, 10, "Dialogue: ") != 0)
        continue;
      std::array<std::string, 9> fields;
      size_t begin = 10;

      for (auto& field : fields) {
        const auto comma = line.find(',', begin);

        if (comma == std::string::npos)
          throw std::runtime_error("Invalid extracted subtitle dialogue");
        field = line.substr(begin, comma - begin);
        begin = comma + 1;
      }
      std::string text;
      bool override = false;

      for (size_t i = begin; i < line.size(); ++i) {
        if (line[i] == '{')
          override = true;
        else if (line[i] == '}')
          override = false;
        else if (!override && line[i] != '\r')
          text += line[i];
      }
      output << "Dialogue: 0," << fields[1] << ',' << fields[2] << ",Band,,0,0,0,,{\\clip(0,"
        << options.height - height << ',' << options.width << ',' << options.height << ")}"
        << text << '\n';
    }
    output.close();
    std::wstring value, escaped;

    // Escape the option value and then the surrounding filter graph
    for (wchar_t ch : std::filesystem::absolute(band).generic_wstring()) {
      if (ch == L'\\' || ch == L'\'' || ch == L':')
        value += L'\\';
      value += ch;
    }
    for (wchar_t ch : value) {
      if (ch == L'\\' || ch == L'\'' || ch == L'[' || ch == L']' || ch == L',' || ch == L';')
        escaped += L'\\';
      escaped += ch;
    }
    return L",setpts=PTS+" + std::to_wstring(options.start) + L"/TB,subtitles=filename=" + escaped +
      L",setpts=PTS-STARTPTS";
  }
}
