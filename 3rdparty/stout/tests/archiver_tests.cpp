// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <string>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif

#include <stout/archiver.hpp>
#include <stout/base64.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

using std::string;


class ArchiverTest : public TemporaryDirectoryTest {};

// No input file should return some error, not read from stdin.
TEST_F(ArchiverTest, ExtractEmptyInputFile)
{
  EXPECT_ERROR(archiver::extract("", ""));
}


// File that does not exist should return some error.
TEST_F(ArchiverTest, ExtractInputFileNotFound)
{
  // Construct a temporary file path that is guarenteed unique.
  Try<string> dir = os::mkdtemp(path::join(sandbox.get(), "XXXXXX"));
  ASSERT_SOME(dir);

  string path = path::join(dir.get(), "ThisFileDoesNotExist");

  EXPECT_ERROR(archiver::extract(path, ""));
}


TEST_F(ArchiverTest, ExtractTarGzFile)
{
  // Construct a hello.tar.gz file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //       22       22 2018-02-21 10:06 hello   Howdy there, partner!\n
  // --------  -------                  ------- ------
  //       22       22                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "H4sICE61jVoAA2hlbGxvLnRhcgDtzjEOwjAQRNGtOcXSU9hx7FyBa0RgK0IR"
      "RsYIcfsEaGhQqggh/VfsFLPFDHEcs6zLzEJon2k7bz7zrQliXdM6Nx/vxVgb"
      "uiBqVt71crvWvqjKKaZ0yOnr31L/p/b5fnxoHWKJO730pZ5j2W5+vQoAAAAA"
      "AAAAAAAAAAAAsGQC2DPIjgAoAAA=").get()));

  // Note: The file does NOT have a .tar.gz extension. We could rename
  // it, but libarchive doesn't care about extensions. It determines
  // the format from the contents of the file. So this is tested here
  // as well.
  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner!\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractTarFile)
{
  // Construct a hello.tar file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // The .tar file, since not compressed, is long. So go through some
  // pains to construct the contents to the file programmatically.
  //
  // We could skip the .tar file test, but it's worth having it.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //    10240    10240 2018-02-21 10:06 hello   Howdy there, partner (.tar)!\n
  // --------  -------                  ------- ------
  //    10240    10240                  1 file

  string tarContents =
      "aGVsbG8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAADAwMDA2NjQAMDAwMTc1MAAwMDAxNzUwADAwMDAwMDAwMDM1"
      "ADEzMjQ1MTA2NTE1ADAxMTY3NAAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhciAgAGplZmZj"
      "b2YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAamVmZmNvZgAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAABIb3dkeSB0aGVyZSwgcGFydG5lciEgKC50YXIp"
      "CgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

  for (int i = 0; i < 214; i++)
  {
      tarContents +=
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  }

  tarContents += "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==";

  // Now write out the .tar file, extract contents, and verify results
  ASSERT_SOME(os::write(path.get(), base64::decode(tarContents).get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner! (.tar)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractZipFile)
{
  // Construct a hello.zip file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      189      189 2018-02-26 15:06 hello   Howdy there, partner! (.zip)\n
  // --------  -------                  ------- ------
  //      189      189                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBAoAAAAAAMZ4WkxFOXeVHQAAAB0AAAAFABwAaGVsbG9VVAkAA+SSlFrk"
      "kpRadXgLAAEE6AMAAAToAwAASG93ZHkgdGhlcmUsIHBhcnRuZXIhICguemlw"
      "KQpQSwECHgMKAAAAAADGeFpMRTl3lR0AAAAdAAAABQAYAAAAAAABAAAAtIEA"
      "AAAAaGVsbG9VVAUAA+SSlFp1eAsAAQToAwAABOgDAABQSwUGAAAAAAEAAQBL"
      "AAAAXAAAAAAA").get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner! (.zip)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractInvalidZipFile)
{
  // Construct a hello.zip file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Write broken zip to file [bad CRC 440a6aa5 (should be af083b2d)].
  //
  //  Length     Date    Time  CRC expected  CRC actual  Name    Content
  // -------- ---------- ----- ------------  ----------  ----    ------
  //       12 2016-03-19 10:08  af083b2d     440a6aa5    world   hello hello\n
  // --------                                            ------- ------
  //       12                                            1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBAoAAAAAABBRc0gtOwivDAAAAAwAAAAFABwAd29ybG9VVAkAAxAX7VYQ"
      "F+1WdXgLAAEE6AMAAARkAAAAaGVsbG8gaGVsbG8KUEsBAh4DCgAAAAAAEFFz"
      "SC07CK8MAAAADAAAAAUAGAAAAAAAAQAAAKSBAAAAAHdvcmxkVVQFAAMQF+1W"
      "dXgLAAEE6AMAAARkAAAAUEsFBgAAAAABAAEASwAAAEsAAAAAAA==").get()));

  EXPECT_ERROR(archiver::extract(path.get(), ""));
}


TEST_F(ArchiverTest, ExtractZipFileWithDuplicatedEntries)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create zip file with duplicates.
  //
  //   Length  Method    Size  Cmpr    Date    Time   CRC-32   Name   Content
  // --------  ------  ------- ---- ---------- ----- --------  ----   -------
  //       1   Stored        1   0% 2016-03-18 22:49 83dcefb7  A          1
  //       1   Stored        1   0% 2016-03-18 22:49 1ad5be0d  A          2
  // --------          -------  ---                           ------- -------
  //       2                2   0%                            2 files

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBBQAAAAAADC2cki379yDAQAAAAEAAAABAAAAQTFQSwMEFAAAAAAAMrZy"
      "SA2+1RoBAAAAAQAAAAEAAABBMlBLAQIUAxQAAAAAADC2cki379yDAQAAAAEA"
      "AAABAAAAAAAAAAAAAACAAQAAAABBUEsBAhQDFAAAAAAAMrZySA2+1RoBAAAA"
      "AQAAAAEAAAAAAAAAAAAAAIABIAAAAEFQSwUGAAAAAAIAAgBeAAAAQAAAAAAA").get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "A");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("2", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractTarXZFile)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create a tar.xz compressed file.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      192      192 2018-02-27 15:34 hello   Hello world (xz)\n
  // --------  -------                  ------- ------
  //      192      192                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
       "/Td6WFoAAATm1rRGAgAhARYAAAB0L+Wj4Cf/AH5dADQZSe6N1/i4P8k3jGgA"
       "rB4mJjQrf8ka7ajHWIxeYZoS+eGuA0Br4ooXZVdW4dnh8GpgDlbdfMQrOOPA"
       "aJE3B9L56mP0ThtjwNuMhhc8/xiXsFOVeUf/xbgcqognut0NZNetr0p+FA/O"
       "K6NqFHAjzSaANcbNj+iFfqY3sC/mAAAAADpda78LIiMIAAGaAYBQAADDUC3D"
       "scRn+wIAAAAABFla").get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Hello world (xz)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractTarBZ2File)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create an tar.bz2 compressed file.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      148      148 2018-02-27 15:34 hello   Hello world (bzip2)\n
  // --------  -------                  ------- ------
  //      148      148                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
       "QlpoOTFBWSZTWZo+haYAAH//hMIRAgBAYH+AAEAACH903pAABAAIIAB0EpEa"
       "IeiMJtAIeRP1BlNCA00AAAA+x2lRZBAgaACRM0TvUjA5RJAR6BfGS3MjVUIh"
       "IUI0Yww9tmran651Du0Hk5ZN4pbSxgs5xlAlIjtgOImyv+auHhIXnipV/xXy"
       "iIHQu5IpwoSE0fQtMA==").get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Hello world (bzip2)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractTarBz2GzFile)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create an tar.bz2.gz compressed file.
  //
  // Verify that archives compressed twice (in this case, .bzip2.gz)
  // work. Libarchive will keep processing until fully extracted.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      185      185 2018-02-27 15:46 hello   Hello world (bzip2)\n
  // --------  -------                  ------- ------
  //      185      185                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
       "H4sICOPtlVoAA2hlbGxvLnRhci5iejIAAZQAa/9CWmg5MUFZJlNZmj6FpgAA"
       "f/+EwhECAEBgf4AAQAAIf3TekAAEAAggAHQSkRoh6Iwm0Ah5E/UGU0IDTQAA"
       "AD7HaVFkECBoAJEzRO9SMDlEkBHoF8ZLcyNVQiEhQjRjDD22atqfrnUO7QeT"
       "lk3iltLGCznGUCUiO2A4ibK/5q4eEheeKlX/FfKIgdC7kinChITR9C0wSQeY"
       "TJQAAAA=").get()));

  EXPECT_SOME(archiver::extract(path.get(), ""));

  string extractedFile = path::join(sandbox.get(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Hello world (bzip2)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractBz2FileFails)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create an .bz2 compressed file.
  //
  // Libarchive does not appear to work without some sort of container
  // (tar or zip or whatever). Verify that a regular file, compressed,
  // will fail.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //       63       63 2018-02-27 17:00 hello   Hello world (bzip2)\n
  // --------  -------                  ------- ------
  //       63       63                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
       "QlpoOTFBWSZTWTMaBKkAAANdgAAQQGAQAABAFiTQkCAAIhGCD1HoUwAE0auv"
       "Imhs/86EgGxdyRThQkDMaBKk").get()));

  EXPECT_ERROR(archiver::extract(path.get(), ""));
}


TEST_F(ArchiverTest, ExtractGzFileFails)
{
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create an .gz compressed file.
  //
  // Libarchive does not appear to work without some sort of container
  // (tar or zip or whatever). Verify that a regular file, compressed,
  // will fail.
  //
  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //       43       43 2018-03-21 16:59 hello   Hello world (gz)\n
  // --------  -------                  ------- ------
  //       43       43                  1 file

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "H4sICNjxsloAA2hlbGxvAPNIzcnJVyjPL8pJUdBIr9LkAgAwtvTdEQAAAA==").get()));

  EXPECT_ERROR(archiver::extract(path.get(), ""));
}


// TODO(josephw): Libarchive currently does not support creating symlinks
// on Windows (hardlinks are fine).
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    ArchiverTest, SYMLINK_ExtractTarGzFileWithLinks)
{
  // Construct a tarball containing a hardlink and symlink.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // This file was generated with the following commands:
  //   echo "I'm a link target" > target
  //   ln target hardlink
  //   ln -s target symlink
  //   tar -czf foo.tar.gz target hardlink symlink
  //   cat foo.tar.gz | base64
  ASSERT_SOME(os::write(path.get(), base64::decode(
      "H4sIADbiKlsAA+3UTQ6CMBCG4a49xezctqWlZ/AYJIL4gxqoMd5efhITF4obM"
      "Mb32UxIJmXIxzRm9SaPalJa69Q56WpIfV+1HZ4H1opJEmN9ql1wok1ig1eipx"
      "1rcGliVrej7E5Nfi6vl1d9bVtRvDln+BJ51B+xWlaSyWF73Evsf4XFtyfCnMq"
      "sXnfpT/mO0f1v9+V5/33bqcTEGS6nP9//5lZNHX+fu/ef5e9tl78L2iix5A8A"
      "AAAAAAAAAAAAADDqDhr0y40AKAAA").get()));

  EXPECT_SOME(archiver::extract(path.get(), dir));

  string extractedFile1 = path::join(dir, "target");
  string extractedFile2 = path::join(dir, "hardlink");
  string extractedFile3 = path::join(dir, "symlink");
  ASSERT_TRUE(os::exists(extractedFile1));
  ASSERT_TRUE(os::exists(extractedFile2));
  ASSERT_TRUE(os::exists(extractedFile3));

  ASSERT_SOME_EQ("I'm a link target\n", os::read(extractedFile1));
  ASSERT_SOME_EQ("I'm a link target\n", os::read(extractedFile2));
  ASSERT_SOME_EQ("I'm a link target\n", os::read(extractedFile3));
}


TEST_F(ArchiverTest, ExtractTarGzFileWithDestinationDir)
{
  // Construct a hello.tar.gz file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> sourcePath = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(sourcePath);

  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //       22       22 2018-02-21 10:06 hello   Howdy there, partner!\n
  // --------  -------                  ------- ------
  //       22       22                  1 file

  ASSERT_SOME(os::write(sourcePath.get(), base64::decode(
      "H4sICE61jVoAA2hlbGxvLnRhcgDtzjEOwjAQRNGtOcXSU9hx7FyBa0RgK0IR"
      "RsYIcfsEaGhQqggh/VfsFLPFDHEcs6zLzEJon2k7bz7zrQliXdM6Nx/vxVgb"
      "uiBqVt71crvWvqjKKaZ0yOnr31L/p/b5fnxoHWKJO730pZ5j2W5+vQoAAAAA"
      "AAAAAAAAAAAAsGQC2DPIjgAoAAA=").get()));

  // Make a destination directory to extract the archive to.
  string destDir = path::join(dir, "somedestination");
  ASSERT_SOME(os::mkdir(destDir));

  // Note: The file does NOT have a .tar.gz extension. We could rename
  // it, but libarchive doesn't care about extensions. It determines
  // the format from the contents of the file. So this is tested here
  // as well.
  //
  // Note: In this test, we extract the file to a destination directory
  // and expect to find it there.
  EXPECT_SOME(archiver::extract(sourcePath.get(), destDir));

  string extractedFile = path::join(destDir, "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner!\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractZipFileWithDestinationDir)
{
  // Construct a hello.zip file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> sourcePath = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(sourcePath);

  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      189      189 2018-02-26 15:06 hello   Howdy there, partner! (.zip)\n
  // --------  -------                  ------- ------
  //      189      189                  1 file

  ASSERT_SOME(os::write(sourcePath.get(), base64::decode(
      "UEsDBAoAAAAAAMZ4WkxFOXeVHQAAAB0AAAAFABwAaGVsbG9VVAkAA+SSlFrk"
      "kpRadXgLAAEE6AMAAAToAwAASG93ZHkgdGhlcmUsIHBhcnRuZXIhICguemlw"
      "KQpQSwECHgMKAAAAAADGeFpMRTl3lR0AAAAdAAAABQAYAAAAAAABAAAAtIEA"
      "AAAAaGVsbG9VVAUAA+SSlFp1eAsAAQToAwAABOgDAABQSwUGAAAAAAEAAQBL"
      "AAAAXAAAAAAA").get()));

  // Make a destination directory to extract the archive to.
  string destDir = path::join(dir, "somedestination");
  ASSERT_SOME(os::mkdir(destDir));

  EXPECT_SOME(archiver::extract(sourcePath.get(), destDir));

  string extractedFile = path::join(destDir, "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner! (.zip)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractZipFileWithLongDestinationDir)
{
  // Construct a hello.zip file that can be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> sourcePath = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(sourcePath);

  //  Length     Size     Date    Time  Name    Content
  // --------  ------- ---------- ----- ----    ------
  //      189      189 2018-02-26 15:06 hello   Howdy there, partner! (.zip)\n
  // --------  -------                  ------- ------
  //      189      189                  1 file

  ASSERT_SOME(os::write(sourcePath.get(), base64::decode(
      "UEsDBAoAAAAAAMZ4WkxFOXeVHQAAAB0AAAAFABwAaGVsbG9VVAkAA+SSlFrk"
      "kpRadXgLAAEE6AMAAAToAwAASG93ZHkgdGhlcmUsIHBhcnRuZXIhICguemlw"
      "KQpQSwECHgMKAAAAAADGeFpMRTl3lR0AAAAdAAAABQAYAAAAAAABAAAAtIEA"
      "AAAAaGVsbG9VVAUAA+SSlFp1eAsAAQToAwAABOgDAABQSwUGAAAAAAEAAQBL"
      "AAAAXAAAAAAA").get()));

  // Make a destination directory to extract the archive to.
  const size_t max_path_length = 260;
  while (dir.length() <= max_path_length) {
    dir = path::join(dir, id::UUID::random().toString());
  }

  EXPECT_TRUE(dir.length() > max_path_length);
  ASSERT_SOME(os::mkdir(dir));

  EXPECT_SOME(archiver::extract(sourcePath.get(), dir));

  string extractedFile = path::join(dir, "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("Howdy there, partner! (.zip)\n", os::read(extractedFile));
}


TEST_F(ArchiverTest, ExtractZipWithDotDot)
{
  // Construct a exploit.zip file that should not be extracted.
  string dir = path::join(sandbox.get(), "somedir");
  ASSERT_SOME(os::mkdir(dir));

  Try<string> path = os::mktemp(path::join(dir, "XXXXXX"));
  ASSERT_SOME(path);

  // Extract from the same directory as the zip file.
  ASSERT_SOME(os::chdir(dir));

  // This is a file contructed with the following Python code:
  //   import zipfile
  //   zip = zipfile.ZipFile("exploit.zip", "w")
  //   zip.writestr("../unsecure_file.txt", "content")
  //   zip.close()

  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBBQAAAAAACZbZk6pMMX+BwAAAAcAAAAUAAAALi4vdW5zZWN1cmVfZmls"
      "ZS50eHRjb250ZW50UEsBAhQDFAAAAAAAJltmTqkwxf4HAAAABwAAABQAAAAA"
      "AAAAAAAAAIABAAAAAC4uL3Vuc2VjdXJlX2ZpbGUudHh0UEsFBgAAAAABAAEA"
      "QgAAADkAAAAAAA==").get()));

  string extractedFile = path::join(sandbox.get(), "unsecure_file.txt");

  EXPECT_ERROR(archiver::extract(path.get(), ""));
  ASSERT_FALSE(os::exists(extractedFile));

  // Just to sanity check, extract again, with the secure flag disabled.
  EXPECT_SOME(archiver::extract(path.get(), "", 0));
  ASSERT_TRUE(os::exists(extractedFile));
}
