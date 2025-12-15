#include "xml_loader.hpp"
#include "tinyxml2.h"
#include <stdexcept>
#include <iostream>

using namespace tinyxml2;

FixDictionary FixDictionaryLoader::LoadBase(const std::string &path) {
    FixDictionary dict;
    XMLDocument doc;

    if (doc.LoadFile(path.c_str()) != XML_SUCCESS) {
        throw std::runtime_error("Failed to load dictionary XML: " + path);
    }

    auto *root = doc.RootElement();
    if (!root) {
        throw std::runtime_error("Invalid FIX dictionary XML: no root element.");
    }

    // ---------------------------
    // Load <fields>
    // ---------------------------
    auto *fields_root = root->FirstChildElement("fields");
    if (fields_root) {
        LoadFields(dict, fields_root);
    }

    // ---------------------------
    // Load <messages>
    // ---------------------------
    auto *messages_root = root->FirstChildElement("messages");
    if (messages_root) {
        LoadMessages(dict, messages_root);
    }

    return dict;
}

// ===========================================================
// FIELD LOADING
// ===========================================================
void FixDictionaryLoader::LoadFields(FixDictionary &dict, XMLElement *fields_root) {
    for (XMLElement *field = fields_root->FirstChildElement("field");
         field != nullptr;
         field = field->NextSiblingElement("field"))
    {
        FixFieldDef def;
        def.tag = field->IntAttribute("number");
        def.name = field->Attribute("name");
        def.type = field->Attribute("type");

        dict.name_to_tag[def.name] = def.tag;

        // Enums
        for (XMLElement *val = field->FirstChildElement("value");
             val != nullptr;
             val = val->NextSiblingElement("value"))
        {
            FixEnum e;
            e.enum_value = val->Attribute("enum");
            e.description = val->Attribute("description");
            def.enums.push_back(e);
        }

        dict.fields[def.tag] = def;
    }
}

// ===========================================================
// GROUP LOADER (recursive)
// ===========================================================
FixGroupDef FixDictionaryLoader::LoadGroup(FixDictionary &dict, XMLElement *group) {
    FixGroupDef g;

    const char *group_name = group->Attribute("name");
    if (!group_name) {
        throw std::runtime_error("Group node missing name attr");
    }

    int count_tag = dict.name_to_tag[group_name];
    g.count_tag = count_tag;

    // group fields
    for (XMLElement *f = group->FirstChildElement("field");
         f != nullptr;
         f = f->NextSiblingElement("field"))
    {
        const char *fname = f->Attribute("name");
        g.field_tags.push_back(dict.name_to_tag[fname]);
    }

    // nested groups
    for (XMLElement *sub = group->FirstChildElement("group");
         sub != nullptr;
         sub = sub->NextSiblingElement("group"))
    {
        FixGroupDef sub_def = LoadGroup(dict, sub);
        g.subgroups[sub_def.count_tag] = sub_def;
    }

    return g;
}

// ===========================================================
// MESSAGE LOADING
// ===========================================================
void FixDictionaryLoader::LoadMessages(FixDictionary &dict, XMLElement *messages_root) {

    for (XMLElement *msg = messages_root->FirstChildElement("message");
         msg != nullptr;
         msg = msg->NextSiblingElement("message"))
    {
        FixMessageDef m;
        m.name     = msg->Attribute("name");
        m.msg_type = msg->Attribute("msgtype");

        // fields
        for (XMLElement *field = msg->FirstChildElement("field");
             field != nullptr;
             field = field->NextSiblingElement("field"))
        {
            const char *fname = field->Attribute("name");
            bool required = strcmp(field->Attribute("required"), "Y") == 0;

            int tag = dict.name_to_tag[fname];
            if (required)
                m.required_fields.push_back(tag);
            else
                m.optional_fields.push_back(tag);
        }

        // repeating groups
        for (XMLElement *group = msg->FirstChildElement("group");
             group != nullptr;
             group = group->NextSiblingElement("group"))
        {
            FixGroupDef g = LoadGroup(dict, group);
            m.groups[g.count_tag] = g;
        }

        dict.messages[m.msg_type] = m;
    }
}

// ===========================================================
// APPLY OVERLAY XML (dialects, custom fields, etc.)
// ===========================================================
void FixDictionaryLoader::ApplyOverlay(FixDictionary &dict, const std::string &path) {
    XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != XML_SUCCESS) {
        throw std::runtime_error("Failed to load overlay XML: " + path);
    }

    auto *root = doc.RootElement();
    if (!root)
        throw std::runtime_error("Overlay XML missing root element.");

    if (auto *fields_root = root->FirstChildElement("fields")) {
        LoadFields(dict, fields_root);
    }

    if (auto *messages_root = root->FirstChildElement("messages")) {
        LoadMessages(dict, messages_root);
    }
}
