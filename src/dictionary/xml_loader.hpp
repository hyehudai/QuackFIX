#pragma once

#include "fix_dictionary.hpp"
#include "tinyxml2.h"
#include <string>

class FixDictionaryLoader {
public:
    static FixDictionary LoadBase(const std::string &path);
    static void ApplyOverlay(FixDictionary &dict, const std::string &path);

private:
    static void LoadFields(FixDictionary &dict, tinyxml2::XMLElement *fields_root);
    static void LoadMessages(FixDictionary &dict, tinyxml2::XMLElement *messages_root);
    static FixGroupDef LoadGroup(FixDictionary &dict, tinyxml2::XMLElement *group);
};
