// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.



#if !defined(BASE_COMMAND_LINE_H_)
#define BASE_COMMAND_LINE_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/logging.h"

class InProcessBrowserTest;

class CommandLine {
 public:
  CommandLine(int argc, const char* const* argv);
  explicit CommandLine(const std::vector<std::string>& argv);

  explicit CommandLine(const std::wstring& program);

  static void Init(int argc, const char* const* argv);

  static void Terminate();

  static const CommandLine* ForCurrentProcess() {
    DCHECK(current_process_commandline_);
    return current_process_commandline_;
  }

  static bool IsInitialized() { return !!current_process_commandline_; }

  bool HasSwitch(const std::wstring& switch_string) const;

  std::wstring GetSwitchValue(const std::wstring& switch_string) const;

  std::vector<std::wstring> GetLooseValues() const;

  const std::vector<std::string>& argv() const { return argv_; }

  std::wstring program() const;

  static std::wstring PrefixedSwitchString(const std::wstring& switch_string);

  static std::wstring PrefixedSwitchStringWithValue(
      const std::wstring& switch_string, const std::wstring& value_string);

  void AppendSwitch(const std::wstring& switch_string);

  void AppendSwitchWithValue(const std::wstring& switch_string,
                             const std::wstring& value_string);

  void AppendLooseValue(const std::wstring& value);


  void AppendArguments(const CommandLine& other, bool include_program);

  void PrependWrapper(const std::wstring& wrapper);

 private:
  friend class InProcessBrowserTest;

  CommandLine() = default;

  static CommandLine* ForCurrentProcessMutable() {
    DCHECK(current_process_commandline_);
    return current_process_commandline_;
  }

  static CommandLine* current_process_commandline_;


  std::vector<std::string> argv_;

  typedef std::string StringType;

  void InitFromArgv();

  static bool IsSwitch(const StringType& parameter_string,
                       std::string* switch_string, StringType* switch_value);

  std::map<std::string, StringType> switches_;

  std::vector<StringType> loose_values_;

};

#endif
