// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: the CA certificates the library's HTTPS clients (httplib over OpenSSL) trust. Android has no
// OpenSSL certificate directory an app can read, so the runtime ships Mozilla's bundle (cacert.pem, build_bionic_deps.sh).
#pragma once

#include <string>

namespace BbNet {
/// BB_CA_BUNDLE, else <the directory above this library's>/share/cacert.pem when it exists, else "" (the
/// library's own defaults). Also exported to OpenSSL as SSL_CERT_FILE by bbnet_init.
const std::string& CaBundlePath();
} // namespace BbNet
