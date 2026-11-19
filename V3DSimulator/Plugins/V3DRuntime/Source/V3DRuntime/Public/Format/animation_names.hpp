// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include <string>
#include <vector>
namespace v3d {
inline std::string animation_key(std::string name) {
    const auto separator=name.find_last_of("|:/\\");
    if(separator!=std::string::npos)name.erase(0,separator+1);
    if(name.size()>3&&(name.substr(0,3)=="AS_"||name.substr(0,3)=="as_"))name.erase(0,3);
    std::string key;for(unsigned char c:name){if(c>='A'&&c<='Z')c+=32;if((c>='a'&&c<='z')||(c>='0'&&c<='9'))key+=char(c);}
    if(key=="walking")return "walk";
    if(key=="running")return "run";
    if(key=="jumping")return "jump";
    if(key=="crouched"||key=="crouching"||key=="crouchidle")return "crouch";
    if(key=="crouchedwalking"||key=="crouchwalking")return "crouchwalk";
    if(key=="swimming")return "swim";
    if(key=="swimmingrun"||key=="swimrun")return "swimfast";
    if(key=="treadingwater")return "swimidle";
    if(key=="swimmingup")return "swimup";
    if(key=="swimmingdown")return "swimdown";
    if(key=="falling")return "fall";
    if(key=="landing")return "land";
    if(key=="flying")return "fly";
    if(key=="floating")return "flyidle";
    if(key=="diving")return "dive";
    if(key=="gettingup"||key=="getup")return "getupback";
    if(key=="standingup")return "getupfront";
    return key;
}
inline std::vector<std::string> animation_candidates(const std::string& name) {
    const auto key=animation_key(name);std::vector<std::string> keys{key};
    if(key=="walkinplace")keys.push_back("walk");
    if(key=="crouchwalk")keys.push_back("crouch");
    if(key=="swimfast"||key=="swimup"||key=="swimdown")keys.push_back("swim");
    if(key=="fallloop")keys.push_back("fall");
    return keys;
}
}
