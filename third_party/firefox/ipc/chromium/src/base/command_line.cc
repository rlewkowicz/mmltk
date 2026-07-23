// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"


#include "base/logging.h"
#include "base/string_piece.h"
#include "base/string_util.h"
#include "base/sys_string_conversions.h"

CommandLine* CommandLine::current_process_commandline_ = nullptr;

const char* const kSwitchPrefixes[] = {"--", "-"};
const char kSwitchTerminator[] = "--";
const char kSwitchValueSeparator[] = "=";


CommandLine::CommandLine(int argc, const char* const* argv) {
  for (int i = 0; i < argc; ++i) argv_.push_back(argv[i]);
  InitFromArgv();
}
CommandLine::CommandLine(const std::vector<std::string>& argv) {
  argv_ = argv;
  InitFromArgv();
}

void CommandLine::InitFromArgv() {
  bool parse_switches = true;
  for (size_t i = 1; i < argv_.size(); ++i) {
    const std::string& arg = argv_[i];

    if (!parse_switches) {
      loose_values_.push_back(arg);
      continue;
    }

    if (arg == kSwitchTerminator) {
      parse_switches = false;
      continue;
    }

    std::string switch_string;
    std::string switch_value;
    if (IsSwitch(arg, &switch_string, &switch_value)) {
      switches_[switch_string] = switch_value;
    } else {
      loose_values_.push_back(arg);
    }
  }
}

CommandLine::CommandLine(const std::wstring& program) {
  argv_.push_back(WideToASCII(program));
}

bool CommandLine::IsSwitch(const StringType& parameter_string,
                           std::string* switch_string,
                           StringType* switch_value) {
  switch_string->clear();
  switch_value->clear();

  for (auto switchPrefix : kSwitchPrefixes) {
    StringType prefix(switchPrefix);
    if (parameter_string.find(prefix) != 0) continue;

    const size_t switch_start = prefix.length();
    const size_t equals_position =
        parameter_string.find(kSwitchValueSeparator, switch_start);
    StringType switch_native;
    if (equals_position == StringType::npos) {
      switch_native = parameter_string.substr(switch_start);
    } else {
      switch_native =
          parameter_string.substr(switch_start, equals_position - switch_start);
      *switch_value = parameter_string.substr(equals_position + 1);
    }
    *switch_string = switch_native;

    return true;
  }

  return false;
}

void CommandLine::Init(int argc, const char* const* argv) {
  DCHECK(current_process_commandline_ == nullptr);
  current_process_commandline_ = new CommandLine(argc, argv);
}

void CommandLine::Terminate() {
  DCHECK(current_process_commandline_ != nullptr);
  delete current_process_commandline_;
  current_process_commandline_ = nullptr;
}

bool CommandLine::HasSwitch(const std::wstring& switch_string) const {
  std::wstring lowercased_switch(switch_string);
  return switches_.find(WideToASCII(lowercased_switch)) != switches_.end();
}

std::wstring CommandLine::GetSwitchValue(
    const std::wstring& switch_string) const {
  std::wstring lowercased_switch(switch_string);

  std::map<std::string, StringType>::const_iterator result =
      switches_.find(WideToASCII(lowercased_switch));

  if (result == switches_.end()) {
    return L"";
  } else {
    return ASCIIToWide(result->second);
  }
}

std::vector<std::wstring> CommandLine::GetLooseValues() const {
  std::vector<std::wstring> values;
  values.reserve(loose_values_.size());
  for (size_t i = 0; i < loose_values_.size(); ++i)
    values.push_back(ASCIIToWide(loose_values_[i]));
  return values;
}
std::wstring CommandLine::program() const {
  DCHECK(argv_.size() > 0);
  return ASCIIToWide(argv_[0]);
}

std::wstring CommandLine::PrefixedSwitchString(
    const std::wstring& switch_string) {
  return StringPrintf(L"%ls%ls", kSwitchPrefixes[0], switch_string.c_str());
}

std::wstring CommandLine::PrefixedSwitchStringWithValue(
    const std::wstring& switch_string, const std::wstring& value_string) {
  if (value_string.empty()) {
    return PrefixedSwitchString(switch_string);
  }

  return StringPrintf(L"%ls%ls%ls%ls", kSwitchPrefixes[0],
                      switch_string.c_str(), kSwitchValueSeparator,
                      value_string.c_str());
}

void CommandLine::AppendSwitch(const std::wstring& switch_string) {
  std::string ascii_switch = WideToASCII(switch_string);
  argv_.push_back(kSwitchPrefixes[0] + ascii_switch);
  switches_[ascii_switch] = "";
}

void CommandLine::AppendSwitchWithValue(const std::wstring& switch_string,
                                        const std::wstring& value_string) {
  std::string ascii_switch = WideToASCII(switch_string);
  std::string ascii_value = WideToASCII(value_string);

  argv_.push_back(kSwitchPrefixes[0] + ascii_switch + kSwitchValueSeparator +
                  ascii_value);
  switches_[ascii_switch] = ascii_value;
}

void CommandLine::AppendLooseValue(const std::wstring& value) {
  argv_.push_back(WideToASCII(value));
}

void CommandLine::AppendArguments(const CommandLine& other,
                                  bool include_program) {
  DCHECK(include_program ? !other.program().empty() : other.program().empty());

  size_t first_arg = include_program ? 0 : 1;
  for (size_t i = first_arg; i < other.argv_.size(); ++i)
    argv_.push_back(other.argv_[i]);
  std::map<std::string, StringType>::const_iterator i;
  for (i = other.switches_.begin(); i != other.switches_.end(); ++i)
    switches_[i->first] = i->second;
}

void CommandLine::PrependWrapper(const std::wstring& wrapper_wide) {
  const std::string wrapper = WideToASCII(wrapper_wide);
  std::vector<std::string> wrapper_and_args;
  SplitString(wrapper, ' ', &wrapper_and_args);
  argv_.insert(argv_.begin(), wrapper_and_args.begin(), wrapper_and_args.end());
}
