#include "q2dz/content_check.hpp"
#include <filesystem>
#include <iostream>
#include <string>
int main(int argc,char** argv){std::filesystem::path root;std::string game="baseq2",map;for(int i=1;i<argc;++i){const std::string k=argv[i];if(i+1>=argc)return 2;const std::string v=argv[++i];if(k=="--root")root=v;else if(k=="--game")game=v;else if(k=="--map")map=v;else return 2;}if(root.empty()||map.empty())return 2;if(q2dz::content_has_map(root,game,map)){std::cout<<"CONTENT_READY game="<<game<<" map="<<map<<"\n";return 0;}std::cout<<"CONTENT_MISSING game="<<game<<" map="<<map<<"\n";return 3;}
