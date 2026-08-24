#include "q2dz/content_check.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace {int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<" CHECK failed: " #x "\n";++failures;}}while(0)
void put32(std::array<unsigned char,64>& b,int n,std::uint32_t v){for(int i=0;i<4;++i)b[n+i]=static_cast<unsigned char>(v>>(i*8));}}
int main(){CHECK(q2dz::safe_qpath("maps/q2dm1.bsp"));CHECK(!q2dz::safe_qpath("../q2dm1"));CHECK(!q2dz::safe_qpath("C:/bad"));const auto root=std::filesystem::temp_directory_path()/"q2dz-content-test";std::filesystem::remove_all(root);std::filesystem::create_directories(root/"baseq2");const auto pak=root/"baseq2"/"pak0.pak";std::ofstream out(pak,std::ios::binary);std::array<unsigned char,12> h{'P','A','C','K'};h[4]=12;h[8]=64;out.write(reinterpret_cast<char*>(h.data()),h.size());std::array<unsigned char,64> row{};const std::string name="maps/q2dm1.bsp";std::copy(name.begin(),name.end(),row.begin());put32(row,56,76);put32(row,60,4);out.write(reinterpret_cast<char*>(row.data()),row.size());out.write("BSP!",4);out.close();CHECK(q2dz::pak_contains(pak,"maps/q2dm1.bsp"));CHECK(q2dz::content_has_map(root,"3zb2","q2dm1"));CHECK(!q2dz::content_has_map(root,"baseq2","missing"));std::filesystem::remove_all(root);return failures?1:0;}
