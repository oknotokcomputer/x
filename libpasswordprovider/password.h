// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBPASSWORDPROVIDER_PASSWORD_H_
#define LIBPASSWORDPROVIDER_PASSWORD_H_

#include <memory>

#include <base/macros.h>

#include "libpasswordprovider/libpasswordprovider_export.h"

namespace password_provider {

// Wrapper around a simple char* string. This class is used to handle allocating
// the memory so that it won't be available in a crash dump and won't be paged
// out to disk. Assumption is that this will be used to hold a user-typed
// password, so the max size will be (sizeof(1 page) - 1). The -1 is to reserve
// space for the null terminator that's added to the end of the string. The
// string is expected to be in UTF-8 format.
//
// TODO(maybelle): Reevaluate this implementation when SecureBlob is fixed
// (https://crbug.com/728047).
class LIBPASSWORDPROVIDER_EXPORT Password {
 public:
  Password() = default;
  ~Password();

  // Create and return Password object from the given file descriptor. |bytes|
  // bytes will be read from |fd| and copied to the Password buffer. |bytes|
  // should not include the null terminator in the count. This function will
  // automatically null-terminate the buffer after reading the data.
  static std::unique_ptr<Password> CreateFromFileDescriptor(int fd,
                                                            size_t bytes);

  // Returns the max size of the buffer.
  size_t max_size() const { return max_size_; }

  // Returns the size of the contents without the null terminator.
  size_t size() const { return size_; }

  // Creates an empty buffer. The buffer will have the appropriate protections
  // against page swapping and dumping in core dumps.
  bool Init();

  // Mutable access to the raw memory. Error if the memory has not been
  // initialized. If a string is being copied to the memory, then it must be
  // null-terminated.
  //
  // TODO(maybelle): Move this to be private when shill tests have been updated
  // to use PasswordTestUtils.
  char* GetMutableRaw();

  // Access to the raw memory. Error if the memory has not been initialized.
  // This buffer is null-terminated.
  const char* GetRaw() const;

  // Sets the size of the contents. The size should be the size of the string
  // without the null terminator.
  void SetSize(size_t size);

 private:
  char* password_ = nullptr;
  size_t buffer_alloc_size_ = 0;
  size_t max_size_ = 0;
  size_t size_ = 0;

  DISALLOW_COPY_AND_ASSIGN(Password);
};

}  // namespace password_provider

#endif  // LIBPASSWORDPROVIDER_PASSWORD_H_
