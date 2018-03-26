// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/environment.h>
#include <base/files/scoped_temp_dir.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <gtest/gtest.h>

#include "vm_tools/garcon/desktop_file.h"

namespace vm_tools {
namespace garcon {

namespace {
struct DesktopFileTestData {
  std::string app_id;
  std::string entry_type;
  std::map<std::string, std::string> locale_name_map;
  std::map<std::string, std::string> locale_comment_map;
  bool no_display;
  std::string icon;
  bool hidden;
  std::vector<std::string> only_show_in;
  std::string try_exec;
  std::string exec;
  std::string path;
  bool terminal;
  std::vector<std::string> mime_types;
  std::vector<std::string> categories;
  std::string startup_wm_class;
};

constexpr char kFilename1[] = "/absolute/file/path";
constexpr char kFilename2[] = "file_path";
constexpr char kUrl1[] = "http://www.example.com/";
constexpr char kUrl2[] = "http://www.example.com.fr/foo/";

class DesktopFileTest : public ::testing::Test {
 public:
  DesktopFileTest() {
    CHECK(temp_dir_.CreateUniqueTempDir());
    apps_dir_ = temp_dir_.GetPath().Append("applications");
    CHECK(base::CreateDirectory(apps_dir_));
    // Set the XDG_DATA_DIRS env var to be the one we created as our
    // temp dir.
    base::Environment::Create()->SetVar("XDG_DATA_DIRS",
                                        temp_dir_.GetPath().value());
  }
  ~DesktopFileTest() override = default;

  base::FilePath WriteDesktopFile(const std::string& file_contents,
                                  const std::string& relative_path) {
    base::FilePath desktop_file_path = apps_dir_.Append(relative_path);
    // If there's a relative path, create any directories in it.
    CHECK(base::CreateDirectory(desktop_file_path.DirName()));
    EXPECT_EQ(file_contents.size(),
              base::WriteFile(desktop_file_path, file_contents.c_str(),
                              file_contents.size()));
    return desktop_file_path;
  }

  std::unique_ptr<DesktopFile> ValidateDesktopFile(
      const std::string& file_contents,
      const std::string& relative_path,
      const DesktopFileTestData& results,
      bool expect_pass) {
    base::FilePath desktop_file_path =
        WriteDesktopFile(file_contents, relative_path);
    std::unique_ptr<DesktopFile> result =
        DesktopFile::ParseDesktopFile(desktop_file_path);
    if (!expect_pass) {
      EXPECT_EQ(nullptr, result.get());
      return nullptr;
    }
    EXPECT_EQ(result->app_id(), results.app_id);
    EXPECT_EQ(result->entry_type(), results.entry_type);
    EXPECT_EQ(result->locale_name_map(), results.locale_name_map);
    EXPECT_EQ(result->locale_comment_map(), results.locale_comment_map);
    EXPECT_EQ(result->no_display(), results.no_display);
    EXPECT_EQ(result->icon(), results.icon);
    EXPECT_EQ(result->hidden(), results.hidden);
    EXPECT_EQ(result->only_show_in(), results.only_show_in);
    EXPECT_EQ(result->try_exec(), results.try_exec);
    EXPECT_EQ(result->exec(), results.exec);
    EXPECT_EQ(result->path(), results.path);
    EXPECT_EQ(result->terminal(), results.terminal);
    EXPECT_EQ(result->mime_types(), results.mime_types);
    EXPECT_EQ(result->categories(), results.categories);
    EXPECT_EQ(result->startup_wm_class(), results.startup_wm_class);
    return result;
  }

 private:
  base::ScopedTempDir temp_dir_;
  base::FilePath apps_dir_;

  DISALLOW_COPY_AND_ASSIGN(DesktopFileTest);
};

}  // namespace

// This tests most parsing, comments, line breaks, multi-strings, simple
// locales and that all the keys we care about are parsed and invalid ones are
// ignored.
TEST_F(DesktopFileTest, AllKeys) {
  ValidateDesktopFile(
      "#Comment1\n"
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Test\n"
      "\n\n"
      "Name[fr]=Test French\n"
      "Comment=Test me out!\n"
      "Comment[es]=Hola for the comment\n"
      "#Comment2\n"
      "#Comment3\n"
      "NoDisplay=true\n"
      "Icon=prettyicon\n"
      "Hidden=true\n"
      "\n\n"
      "OnlyShowIn=KDE;Gnome;\n"
      "TryExec=mybinary\n"
      "UnknownKey=trickster\n"
      "Exec=mybinary %F\n"
      "#Comment4\n"
      "Path=/usr/local/bin\n"
      "Terminal=true\n"
      "MimeType=text/plain;foo/x-java\n"
      "Categories=Magic;Playtime\n"
      "StartupWMClass=classy\n",
      "test.desktop",
      {
          "test",
          "Application",
          {std::make_pair("", "Test"), std::make_pair("fr", "Test French")},
          {std::make_pair("", "Test me out!"),
           std::make_pair("es", "Hola for the comment")},
          true,
          "prettyicon",
          true,
          {"KDE", "Gnome"},
          "mybinary",
          "mybinary %F",
          "/usr/local/bin",
          true,
          {"text/plain", "foo/x-java"},
          {"Magic", "Playtime"},
          "classy",
      },
      true);
}

TEST_F(DesktopFileTest, Locales) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=LocaleTest\n"
      "Name[sr]=Test sr foo\n"
      "Name[sr_YU]=Test sr underscore YU foo\n"
      "Name[sr_YU@Latn]=Test sr underscore YU at Latn foo\n"
      "Name[sr@Latn]=Test sr at Latn foo\n"
      "Name[ab]=Test ab foo\n"
      "Name[ab_cd]=Test ab underscore cd foo\n"
      "Name[ab_cd@xyz]=Test ab underscore cd at xyz foo\n"
      "Name[ab@xyz]=Test ab at xyz foo\n",
      "locales.desktop",
      {
          "locales",
          "Application",
          {
              std::make_pair("", "LocaleTest"),
              std::make_pair("sr", "Test sr foo"),
              std::make_pair("sr_YU", "Test sr underscore YU foo"),
              std::make_pair("sr_YU@Latn", "Test sr underscore YU at Latn foo"),
              std::make_pair("sr@Latn", "Test sr at Latn foo"),
              std::make_pair("ab", "Test ab foo"),
              std::make_pair("ab_cd", "Test ab underscore cd foo"),
              std::make_pair("ab_cd@xyz", "Test ab underscore cd at xyz foo"),
              std::make_pair("ab@xyz", "Test ab at xyz foo"),
          },
      },
      true);
}

TEST_F(DesktopFileTest, Escaping) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Test \\\"Quoted\\\" \\t tab \\s space \\r CR \\n newline \\\\ "
      "backslash\n"
      "OnlyShowIn=semicolon\\;;;AfterEmpty;Another\\;Semi;\n",
      "EscapeMe.desktop",
      {
          "EscapeMe",
          "Application",
          {
              std::make_pair("",
                             "Test \"Quoted\" \t tab   space \r CR \n newline "
                             "\\ backslash"),
          },
          {},
          false,
          "",
          false,
          {"semicolon;", "", "AfterEmpty", "Another;Semi"},
      },
      true);
}

TEST_F(DesktopFileTest, WhitespaceRemoval) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type =Application \n"
      "Name = TestW\n",
      "whitespace.desktop",
      {
          "whitespace", "Application", {std::make_pair("", "TestW")},
      },
      true);
}

TEST_F(DesktopFileTest, Types) {
  EXPECT_TRUE(ValidateDesktopFile("[Desktop Entry]\n"
                                  "Type=Application\n"
                                  "Name=TestApplication\n",
                                  "ApplicationTest.desktop",
                                  {
                                      "ApplicationTest",
                                      "Application",
                                      {std::make_pair("", "TestApplication")},
                                  },
                                  true)
                  ->IsApplication());
  EXPECT_FALSE(ValidateDesktopFile("[Desktop Entry]\n"
                                   "Type=Directory\n"
                                   "Name=TestDirectory\n",
                                   "DirectoryTest.desktop",
                                   {
                                       "DirectoryTest",
                                       "Directory",
                                       {std::make_pair("", "TestDirectory")},
                                   },
                                   true)
                   ->IsApplication());
  EXPECT_FALSE(ValidateDesktopFile(
                   "[Desktop Entry]\n"
                   "Type=Link\n"
                   "Name=TestLink\n",
                   "LinkTest.desktop",
                   {
                       "LinkTest", "Link", {std::make_pair("", "TestLink")},
                   },
                   true)
                   ->IsApplication());
  // Now try an invalid type, which should fail
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=FakeType\n"
      "Name=TestLink\n",
      "faketype.desktop", {}, false);
}

TEST_F(DesktopFileTest, RelativePathConversion) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Test\n",
      "foo/bar_fun/mad.desktop",
      {
          "foo-bar_fun-mad", "Application", {std::make_pair("", "Test")},
      },
      true);
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Test\n",
      "foo/applications/bar.desktop",
      {
          "foo-applications-bar", "Application", {std::make_pair("", "Test")},
      },
      true);
}

TEST_F(DesktopFileTest, IgnoreOtherGroups) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=TestApplication\n"
      "[Desktop Action Foo]\n"
      "Type=Directory\n"
      "Name=BadApplication\n",
      "ApplicationTest.desktop",
      {
          "ApplicationTest",
          "Application",
          {std::make_pair("", "TestApplication")},
      },
      true);
}

TEST_F(DesktopFileTest, FindDesktopFile) {
  base::FilePath test_path = WriteDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=TestApplication\n",
      "FindTest.desktop");
  EXPECT_EQ(test_path.value(),
            DesktopFile::FindFileForDesktopId("FindTest").value());
  test_path = WriteDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=TestApplication\n",
      "find/me/in/subdir.desktop");
  EXPECT_EQ(test_path.value(),
            DesktopFile::FindFileForDesktopId("find-me-in-subdir").value());
  test_path = WriteDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=TestApplication\n",
      "test/applications/subdir.desktop");
  EXPECT_EQ(
      test_path.value(),
      DesktopFile::FindFileForDesktopId("test-applications-subdir").value());
}

TEST_F(DesktopFileTest, GenerateArgvNoArgs) {
  EXPECT_EQ(ValidateDesktopFile("[Desktop Entry]\n"
                                "Type=Application\n"
                                "Name=Vim\n"
                                "Exec=/usr/bin/vim\n",
                                "vim.desktop",
                                {
                                    "vim",
                                    "Application",
                                    {std::make_pair("", "Vim")},
                                    {},
                                    false,
                                    "",
                                    false,
                                    {},
                                    "",
                                    "/usr/bin/vim",
                                },
                                true)
                ->GenerateArgvWithFiles(std::vector<std::string>()),
            std::vector<std::string>({"/usr/bin/vim"}));
}

TEST_F(DesktopFileTest, GenerateArgvComplexArgs) {
  std::unique_ptr<DesktopFile> desktop_file = ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=Foobar\n"
      "Icon=fooicon\n"
      "Exec=foobar.bin --singlefile=%f MultiFile %F --single_url %u "
      "multi-url %U Icon %i Name %c DesktopPath %k\n",
      "foobar.desktop",
      {
          "foobar",
          "Application",
          {std::make_pair("", "Foobar")},
          {},
          false,
          "fooicon",
          false,
          {},
          "",
          "foobar.bin --singlefile=%f MultiFile %F --single_url %u "
          "multi-url %U Icon %i Name %c DesktopPath %k",
      },
      true);
  EXPECT_EQ(desktop_file->GenerateArgvWithFiles(std::vector<std::string>(
                {kFilename1, kFilename2, kUrl1, kUrl2})),
            std::vector<std::string>({
                "foobar.bin",
                std::string("--singlefile=").append(kFilename1),
                "MultiFile",
                kFilename1,
                kFilename2,
                kUrl1,
                kUrl2,
                "--single_url",
                kFilename1,
                "multi-url",
                kFilename1,
                kFilename2,
                kUrl1,
                kUrl2,
                "Icon",
                "--icon",
                "fooicon",
                "Name",
                "Foobar",
                "DesktopPath",
                desktop_file->file_path().value(),
            }));
}

TEST_F(DesktopFileTest, GenerateArgvWithQuotingAndEscaping) {
  EXPECT_EQ(ValidateDesktopFile(
                u8R"xxx(
                [Desktop Entry]
                Type=Application
                Name=QuoteMaster
                Exec=quote-master %% \"A B %f %i C \\" B \\\\ \" \"C D\"
                )xxx",
                "quoter.desktop",
                {
                    "quoter",
                    "Application",
                    {std::make_pair("", "QuoteMaster")},
                    {},
                    false,
                    "",
                    false,
                    {},
                    "",
                    u8R"xxx(quote-master %% "A B %f %i C \" B \\ " "C D")xxx",
                },
                true)
                ->GenerateArgvWithFiles(std::vector<std::string>()),
            std::vector<std::string>(
                {"quote-master", "%", u8R"xxx(A B %f %i C " B \ )xxx", "C D"}));
}

TEST_F(DesktopFileTest, MissingNameFails) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name[fr]=AlsoNeedNoLocaleName\n",
      "MissingName.desktop", {}, false);
}

TEST_F(DesktopFileTest, InvalidFileExtensionFails) {
  ValidateDesktopFile(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=TestName\n",
      "badextension.notdesktop", {}, false);
}

}  // namespace garcon
}  // namespace vm_tools
