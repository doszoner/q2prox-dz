#include "q2dz/content_check.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>
namespace q2dz { namespace {
std::uint32_t le32(const unsigned char* p){return std::uint32_t(p[0])|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
std::string lower(std::string_view v){std::string o(v);for(char& c:o)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return o;}
bool directory_has_map(const std::filesystem::path& dir,const std::string& qpath){if(std::filesystem::is_regular_file(dir/std::filesystem::path(qpath)))return true;std::vector<std::filesystem::path> paks;std::error_code ec;if(!std::filesystem::is_directory(dir,ec))return false;for(const auto& e:std::filesystem::directory_iterator(dir,ec))if(e.is_regular_file()&&lower(e.path().extension().string())==".pak")paks.push_back(e.path());std::sort(paks.begin(),paks.end());for(const auto& p:paks)if(pak_contains(p,qpath))return true;return false;}
}
bool safe_qpath(std::string_view p) noexcept {if(p.empty()||p.size()>255||p.front()=='/'||p.back()=='/'||p.find('\\')!=p.npos||p.find(':')!=p.npos)return false;std::size_t s=0;while(s<p.size()){const auto e=p.find('/',s);const auto part=p.substr(s,e==p.npos?p.size()-s:e-s);if(part.empty()||part=="."||part=="..")return false;s=e==p.npos?p.size():e+1;}return true;}
bool pak_contains(const std::filesystem::path& pak,std::string_view qpath){if(!safe_qpath(qpath))return false;std::ifstream in(pak,std::ios::binary);if(!in)return false;in.seekg(0,std::ios::end);const auto size=in.tellg();if(size<12)return false;in.seekg(0);std::array<unsigned char,12> h{};in.read(reinterpret_cast<char*>(h.data()),h.size());if(!in||std::string_view(reinterpret_cast<char*>(h.data()),4)!="PACK")return false;const auto off=le32(h.data()+4),len=le32(h.data()+8);if(len%64||std::uint64_t(off)+len>std::uint64_t(size))return false;in.seekg(off);std::array<unsigned char,64> row{};const auto wanted=lower(qpath);for(std::uint32_t pos=0;pos<len;pos+=64){in.read(reinterpret_cast<char*>(row.data()),row.size());if(!in)return false;const auto nul=std::find(row.begin(),row.begin()+56,0);if(nul==row.begin()+56)return false;const std::string name(row.begin(),nul);const auto file_off=le32(row.data()+56),file_len=le32(row.data()+60);if(!safe_qpath(name)||std::uint64_t(file_off)+file_len>std::uint64_t(size))return false;if(lower(name)==wanted)return true;}return false;}
bool content_has_map(const std::filesystem::path& root,std::string_view game,std::string_view map){if(!safe_qpath(game)||game.find('/')!=game.npos||!safe_qpath(map)||map.find('/')!=map.npos)return false;const auto qpath="maps/"+std::string(map)+".bsp";if(directory_has_map(root/std::filesystem::path(game),qpath))return true;return game!="baseq2"&&directory_has_map(root/"baseq2",qpath);}
}
