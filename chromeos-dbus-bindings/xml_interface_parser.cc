// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos-dbus-bindings/xml_interface_parser.h"

#include <utility>

#include <base/file_util.h>
#include <base/files/file_path.h>
#include <base/logging.h>
#include <base/stl_util.h>

using std::string;
using std::vector;

namespace chromeos_dbus_bindings {

// static
const char XmlInterfaceParser::kArgumentTag[] = "arg";
const char XmlInterfaceParser::kInterfaceTag[] = "interface";
const char XmlInterfaceParser::kMethodTag[] = "method";
const char XmlInterfaceParser::kNodeTag[] = "node";
const char XmlInterfaceParser::kSignalTag[] = "signal";
const char XmlInterfaceParser::kNameAttribute[] = "name";
const char XmlInterfaceParser::kTypeAttribute[] = "type";
const char XmlInterfaceParser::kDirectionAttribute[] = "direction";
const char XmlInterfaceParser::kArgumentDirectionIn[] = "in";
const char XmlInterfaceParser::kArgumentDirectionOut[] = "out";

bool XmlInterfaceParser::ParseXmlInterfaceFile(
    const base::FilePath& interface_file) {
  string contents;
  if (!base::ReadFileToString(interface_file, &contents)) {
    LOG(ERROR) << "Failed to read file " << interface_file.value();
    return false;
  }
  auto parser = XML_ParserCreate(nullptr);
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser,
                        &XmlInterfaceParser::HandleElementStart,
                        &XmlInterfaceParser::HandleElementEnd);
  const int kIsFinal = XML_TRUE;

  element_path_.clear();
  XML_Status res = XML_Parse(parser,
                             contents.c_str(),
                             contents.size(),
                             kIsFinal);
  XML_ParserFree(parser);

  if (res != XML_STATUS_OK) {
    LOG(ERROR) << "XML parse failure";
    return false;
  }

  CHECK(element_path_.empty());
  return true;
}

void XmlInterfaceParser::OnOpenElement(
    const string& element_name, const XmlAttributeMap& attributes) {
  element_path_.push_back(element_name);
  if (element_path_ == vector<string> { kNodeTag, kInterfaceTag }) {
    string interface_name = GetValidatedElementName(attributes, kInterfaceTag);
    CHECK(interface_.name.empty())
        << "Found a second interface named " << interface_name << ". "
        << "Interface " << interface_.name << " has already been parsed.";
    interface_.name = interface_name;
  } else if (element_path_ == vector<string> {
                 kNodeTag, kInterfaceTag, kMethodTag }) {
    interface_.methods.push_back(
        Interface::Method(GetValidatedElementName(attributes, kMethodTag)));
  } else if (element_path_ == vector<string> {
                 kNodeTag, kInterfaceTag, kMethodTag, kArgumentTag }) {
    AddMethodArgument(attributes);
  } else if (element_path_ == vector<string> {
                 kNodeTag, kInterfaceTag, kSignalTag }) {
    interface_.signals.push_back(
        Interface::Signal(GetValidatedElementName(attributes, kSignalTag)));
  } else if (element_path_ == vector<string> {
                 kNodeTag, kInterfaceTag, kSignalTag, kArgumentTag }) {
    AddSignalArgument(attributes);
  }
}

void XmlInterfaceParser::AddMethodArgument(const XmlAttributeMap& attributes) {
  CHECK(!interface_.methods.empty())
      << " we have a method argument but the interface has no methods";
  string argument_direction;
  bool is_direction_paramter_present = GetElementAttribute(
      attributes,
      string(kMethodTag) + " " + kArgumentTag,
      kDirectionAttribute,
      &argument_direction);
  vector<Interface::Argument>* argument_list = nullptr;
  if (!is_direction_paramter_present ||
      argument_direction == kArgumentDirectionIn) {
    argument_list = &interface_.methods.back().input_arguments;
  } else if (argument_direction == kArgumentDirectionOut) {
    argument_list = &interface_.methods.back().output_arguments;
  } else {
    LOG(FATAL) << "Unknown method argument direction " << argument_direction;
  }
  argument_list->push_back(ParseArgument(attributes, kMethodTag));
}

void XmlInterfaceParser::AddSignalArgument(const XmlAttributeMap& attributes) {
  CHECK(interface_.signals.size())
      << " we have a signal argument but the interface has no signals";
  interface_.signals.back().arguments.push_back(
      ParseArgument(attributes, kSignalTag));
}

void XmlInterfaceParser::OnCloseElement(const string& element_name) {
  VLOG(1) << "Close Element " << element_name;
  CHECK(!element_path_.empty());
  CHECK_EQ(element_path_.back(), element_name);
  element_path_.pop_back();
}

// static
bool XmlInterfaceParser::GetElementAttribute(
    const XmlAttributeMap& attributes,
    const string& element_type,
    const string& element_key,
    string* element_value) {
  if (!ContainsKey(attributes, element_key)) {
    return false;
  }
  *element_value = attributes.find(element_key)->second;
  VLOG(1) << "Got " << element_type << " element with "
          << element_key << " = " << *element_value;
  return true;
}

// static
string XmlInterfaceParser::GetValidatedElementAttribute(
    const XmlAttributeMap& attributes,
    const string& element_type,
    const string& element_key) {
  string element_value;
  CHECK(GetElementAttribute(attributes,
                            element_type,
                            element_key,
                            &element_value))
      << element_type << " does not contain a " << element_key << " attribute";
  CHECK(!element_value.empty()) << element_type << " " << element_key
                              << " attribute is empty";
  return element_value;
}

// static
string XmlInterfaceParser::GetValidatedElementName(
    const XmlAttributeMap& attributes,
    const string& element_type) {
  return GetValidatedElementAttribute(attributes, element_type, kNameAttribute);
}

// static
Interface::Argument XmlInterfaceParser::ParseArgument(
    const XmlAttributeMap& attributes, const string& element_type) {
  string element_and_argument = element_type + " " + kArgumentTag;
  string argument_name;
  // Since the "name" field is optional, use the un-validated variant.
  GetElementAttribute(attributes,
                      element_and_argument,
                      kNameAttribute,
                      &argument_name);

  string argument_type = GetValidatedElementAttribute(
      attributes, element_and_argument, kTypeAttribute);
  return Interface::Argument(argument_name, argument_type);
}

// static
void XmlInterfaceParser::HandleElementStart(void* user_data,
                                            const XML_Char* element,
                                            const XML_Char** attr) {
  XmlAttributeMap attributes;
  if (attr != nullptr) {
    for (size_t n = 0; attr[n] != nullptr && attr[n+1] != nullptr; n += 2) {
      auto key = attr[n];
      auto value = attr[n + 1];
      attributes.insert(std::make_pair(key, value));
    }
  }
  auto parser = reinterpret_cast<XmlInterfaceParser*>(user_data);
  parser->OnOpenElement(element, attributes);
}

// static
void XmlInterfaceParser::HandleElementEnd(void* user_data,
                                          const XML_Char* element) {
  auto parser = reinterpret_cast<XmlInterfaceParser*>(user_data);
  parser->OnCloseElement(element);
}

}  // namespace chromeos_dbus_bindings
