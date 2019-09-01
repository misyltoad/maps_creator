#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <string>
#include <array>
#include <vector>
#include <iostream>
#include <fstream>

#include <filesystem>
#include <bitset>

#ifdef WIN32
#include <Windows.h>
#endif

namespace TextColors {
  constexpr char* Red     = "\u001b[31;1m";
  constexpr char* Green   = "\u001b[32;1m";
  constexpr char* Yellow  = "\u001b[33;1m";
  constexpr char* Blue    = "\u001b[34;1m";
  constexpr char* Cyan    = "\u001b[36;1m";
  constexpr char* White   = "\u001b[37;1m";

  void EnableColors() {
#ifdef WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
  }
}

namespace MapsCreator {

  namespace ChannelIndices {
    enum ChannelIndex : uint32_t {
      Red   = 0,
      Green = 1,
      Blue  = 2,
      Alpha = 3,
      Count
    };
  }
  using ChannelIndex = ChannelIndices::ChannelIndex;

  namespace Maps {
    enum Map : uint32_t {
      Map1 = 1,
      Map2 = 2,
      Map3,
      Count,
    };
  }
  using Map = Maps::Map;

  enum class Maps1AlphaState {
    None,
    Occlusion,
    SelfIllum,
    TintMask,
    Subsurface
  };

  struct MapChannel {
    Maps1AlphaState           AlphaState;
    std::string               Name;
    uint8_t                   DefaultValue;

    Map                       MapIndex;
    
    std::vector<ChannelIndex> Indices;
  };


  std::string ImageName(const std::string& textureName, const MapChannel& channel) {
    return textureName + "_" + channel.Name + ".png";
  }


  std::string MapName(const std::string& textureName, uint32_t idx) {
    return textureName + "_maps" + std::to_string(idx);
  }


  std::string MapFileName(const std::string& textureName, uint32_t idx) {
    return MapName(textureName, idx) + ".png";
  }


  std::string VMTName(const std::string& textureName) {
    return textureName + ".vmt";
  }


  static std::array<MapChannel, 8> g_MapChannels = {{
    {Maps1AlphaState::None,      "albedo",    255,                     Maps::Map1, {ChannelIndices::Red, ChannelIndices::Green, ChannelIndices::Blue}},
    {Maps1AlphaState::None,      "alpha",     255,                     Maps::Map1, {ChannelIndices::Alpha}},
    {Maps1AlphaState::None,      "roughness", uint8_t(0.95f * 255.0f), Maps::Map2, {ChannelIndices::Red}},
    {Maps1AlphaState::None,      "metalness", uint8_t(0.04f * 255.0f), Maps::Map2, {ChannelIndices::Blue}},
    {Maps1AlphaState::None,      "normal",    127,                     Maps::Map2, {ChannelIndices::Green, ChannelIndices::Alpha}},
    {Maps1AlphaState::TintMask,  "tintmask",  255,                     Maps::Map3, {ChannelIndices::Red}},
    {Maps1AlphaState::Occlusion, "occlusion", 255,                     Maps::Map3, {ChannelIndices::Green}},
    {Maps1AlphaState::TintMask,  "selfillum", 255,                     Maps::Map3, {ChannelIndices::Blue}},
  }};

  auto& g_AlphaChannel = g_MapChannels[1];

  std::string g_UsageString =
R"(maps_creator.exe <texture_name>
Will output maps1, maps2, [and maps3 if required] in the most efficient way for a given material.
This will read files with <texture_name>_channel, where channel can be one of:)";

  void PrintUsage() {
    std::cout << g_UsageString << std::endl;
    for (const auto& channel : g_MapChannels)
      std::cout << channel.Name << std::endl;
  }


  Maps1AlphaState DetermineAlphaState(const std::string& textureName) {
    for (const auto& channel : g_MapChannels) {
      if (channel.AlphaState != Maps1AlphaState::None && std::filesystem::exists(ImageName(textureName, channel)))
        return channel.AlphaState;
    }

    return Maps1AlphaState::None;
  }


  void FixupAlphaState(Maps1AlphaState alphaState) {
    if (alphaState == Maps1AlphaState::None)
      return;

    for (auto& channel : g_MapChannels) {
      if (channel.AlphaState == alphaState) {
        std::swap(channel.MapIndex, g_AlphaChannel.MapIndex);
        std::swap(channel.Indices,  g_AlphaChannel.Indices);
      }
    }
  }


  void CopyData(uint8_t* dst, uint8_t* src, int width, int height, ChannelIndex dstIndex, ChannelIndex srcIndex, uint8_t defaultValue) {
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        uint32_t pixelStart = y * width * ChannelIndices::Count
                            + x * ChannelIndices::Count;
        dst[pixelStart + dstIndex] = src != nullptr ? src[pixelStart + srcIndex] : defaultValue;
      }
    }
  }


  void OutputVMT(Maps1AlphaState state, const std::string& textureName, std::bitset<Maps::Count> writtenMaps) {
    std::string filename = VMTName(textureName);

    auto file = std::ofstream(filename);

    file << R"("PBRStandard")"                  << "\n";
    file << "{"                                 << "\n";
    file << "  $maps1alpha " << uint32_t(state) << "\n";

    for (uint32_t i = 0; i < writtenMaps.size(); i++) {
      if (!writtenMaps[i])
        continue;

      std::string mapName = MapName(textureName, i);

      file << "  $maps" << i << " \"" << mapName  << "\"\n";
    }

    file << "}"                                 << "\n";
  }


  bool OutputImage(const std::string& textureName) {
    auto alphaState = DetermineAlphaState(textureName);
    FixupAlphaState(alphaState);

    std::bitset<Maps::Count> writtenMaps = { };

    for (uint32_t map = Maps::Map1; map < Maps::Count; map++) {
      int lastWidth = 0, lastHeight = 0;
      uint8_t* mapData = nullptr;

      std::string mapFileName = MapFileName(textureName, map);

      bool usedSomethingOtherThanDefaults = false;
      for (const auto& channel : g_MapChannels) {
        if (channel.MapIndex != map)
          continue;

        std::string channelFileName = ImageName(textureName, channel);

        int width, height, channels;
        uint8_t* channelData = stbi_load(channelFileName.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        usedSomethingOtherThanDefaults |= channelData != nullptr;
        
        if (mapData != nullptr) {
          if (lastWidth != width || lastHeight != height) {
            std::cout << TextColors::Red << "Mismatched image widths within channels going to a single map file!" << TextColors::White << std::endl;
            std::cout << lastWidth << "x" << lastHeight << " vs. " << width <<  "x" << height << std::endl;

            if (channelData != nullptr)
              stbi_image_free(channelData);
            return false;
          }
        }
        else
          mapData = new uint8_t[width * height * ChannelIndices::Count];

        lastWidth  = width;
        lastHeight = height;

        for (uint32_t i = 0; i < channel.Indices.size(); i++) {
          if (channelData != nullptr)
            std::cout << "Found " << channel.Name << " putting in map " << map << " channel " << channel.Indices[i] << std::endl;
          else
            std::cout << TextColors::Yellow << "Didn't find " << channel.Name << " putting default " << uint32_t(channel.DefaultValue) << " in map " << map << " channel " << channel.Indices[i] << TextColors::White << std::endl;

          CopyData(mapData, channelData, width, height, channel.Indices[i], ChannelIndex(i), channel.DefaultValue);
        }

        if (channelData != nullptr)
          stbi_image_free(channelData);
      }

      if (mapData != nullptr) {
        if (!usedSomethingOtherThanDefaults) {
          std::cout << TextColors::Blue << "Discarding map " << map << " as it contains only defaults" << TextColors::White << std::endl;
          continue;
        }

        writtenMaps[map] = true;

        if (!stbi_write_png(mapFileName.c_str(), lastWidth, lastHeight, STBI_rgb_alpha, mapData, 0)) {
          std::cout << TextColors::Red << "Failed to write file: " << mapFileName << TextColors::White << std::endl;
          return false;
        }
      }
    }

    OutputVMT(alphaState, textureName, writtenMaps);

    std::cout << TextColors::Green << "Done! You now need to convert to .vtf, and fixup the paths in your .vmt!" << TextColors::White << std::endl;

    return true;
  }

  bool Start(const std::string& textureName) {
    if (textureName.empty()) {
      std::cout << TextColors::Yellow << "You need to specify a texture name." << TextColors::White << std::endl;
      PrintUsage();
      return true;
    }

    return OutputImage(textureName);
  }

}

int main(int argc, char** argv) {
  TextColors::EnableColors();

  std::string textureName = argc >= 2 ? argv[1] : "";

  std::cout << TextColors::Cyan << "MapsCreator by Joshua Ashton" << TextColors::White << std::endl;

  if (!MapsCreator::Start(textureName)) {
    std::cout << TextColors::Red << "An error occured. Tell Josh if you're sure you didn't fuck up!" << TextColors::White << std::endl;
    return 1;
  }

  return 0;
}