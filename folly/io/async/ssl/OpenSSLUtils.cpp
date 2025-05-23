/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/ssl/OpenSSLUtils.h>

#include <unordered_map>

#include <glog/logging.h>

#include <folly/ScopeGuard.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/ssl/detail/OpenSSLSession.h>

namespace {
#ifdef OPENSSL_IS_BORINGSSL
// BoringSSL doesn't (as of May 2016) export the equivalent
// of BIO_sock_should_retry, so this is one way around it :(
static int boringssl_bio_fd_should_retry(int err);
#endif

} // namespace

namespace folly {
namespace ssl {

bool OpenSSLUtils::getTLSMasterKey(
    const SSL_SESSION* session, MutableByteRange keyOut) {
  auto size = SSL_SESSION_get_master_key(session, nullptr, 0);
  if (size == keyOut.size()) {
    return SSL_SESSION_get_master_key(session, keyOut.begin(), keyOut.size());
  }
  return false;
}

bool OpenSSLUtils::getTLSMasterKey(
    const std::shared_ptr<SSLSession> session, MutableByteRange keyOut) {
  auto openSSLSession =
      std::dynamic_pointer_cast<folly::ssl::detail::OpenSSLSession>(session);
  if (openSSLSession) {
    auto rawSessionPtr = openSSLSession->getActiveSession();
    SSL_SESSION* rawSession = rawSessionPtr.get();
    if (rawSession) {
      return OpenSSLUtils::getTLSMasterKey(rawSession, keyOut);
    }
  }
  return false;
}

bool OpenSSLUtils::getTLSClientRandom(
    const SSL* ssl, MutableByteRange randomOut) {
  auto size = SSL_get_client_random(ssl, nullptr, 0);
  if (size == randomOut.size()) {
    return SSL_get_client_random(ssl, randomOut.begin(), randomOut.size());
  }
  return false;
}

bool OpenSSLUtils::getPeerAddressFromX509StoreCtx(
    X509_STORE_CTX* ctx, sockaddr_storage* addrStorage, socklen_t* addrLen) {
  // Grab the ssl idx and then the ssl object so that we can get the peer
  // name to compare against the ips in the subjectAltName
  auto sslIdx = SSL_get_ex_data_X509_STORE_CTX_idx();
  auto ssl = reinterpret_cast<SSL*>(X509_STORE_CTX_get_ex_data(ctx, sslIdx));
  int fd = SSL_get_fd(ssl);
  if (fd < 0) {
    LOG(ERROR) << "Inexplicably couldn't get fd from SSL";
    return false;
  }

  *addrLen = sizeof(*addrStorage);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(addrStorage), addrLen) != 0) {
    PLOG(ERROR) << "Unable to get peer name";
    return false;
  }
  CHECK(*addrLen <= sizeof(*addrStorage));
  return true;
}

bool OpenSSLUtils::validatePeerCertNames(
    X509* cert, const sockaddr* addr, socklen_t /* addrLen */) {
  // Try to extract the names within the SAN extension from the certificate
  auto altNames = reinterpret_cast<STACK_OF(GENERAL_NAME)*>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  SCOPE_EXIT {
    if (altNames != nullptr) {
      sk_GENERAL_NAME_pop_free(altNames, GENERAL_NAME_free);
    }
  };
  if (altNames == nullptr) {
    LOG(WARNING) << "No subjectAltName provided and we only support ip auth";
    return false;
  }

  const sockaddr_in* addr4 = nullptr;
  const sockaddr_in6* addr6 = nullptr;
  if (addr != nullptr) {
    if (addr->sa_family == AF_INET) {
      addr4 = reinterpret_cast<const sockaddr_in*>(addr);
    } else if (addr->sa_family == AF_INET6) {
      addr6 = reinterpret_cast<const sockaddr_in6*>(addr);
    } else {
      LOG(FATAL) << "Unsupported sockaddr family: " << addr->sa_family;
    }
  }

  for (int i = 0; i < sk_GENERAL_NAME_num(altNames); i++) {
    auto name = sk_GENERAL_NAME_value(altNames, i);
    if ((addr4 != nullptr || addr6 != nullptr) && name->type == GEN_IPADD) {
      // Extra const-ness for paranoia
      unsigned char const* const rawIpStr = name->d.iPAddress->data;
      auto const rawIpLen = size_t(name->d.iPAddress->length);

      if (rawIpLen == 4 && addr4 != nullptr) {
        if (::memcmp(rawIpStr, &addr4->sin_addr, rawIpLen) == 0) {
          return true;
        }
      } else if (rawIpLen == 16 && addr6 != nullptr) {
        if (::memcmp(rawIpStr, &addr6->sin6_addr, rawIpLen) == 0) {
          return true;
        }
      } else if (rawIpLen != 4 && rawIpLen != 16) {
        LOG(WARNING) << "Unexpected IP length: " << rawIpLen;
      }
    }
  }

  LOG(WARNING) << "Unable to match client cert against alt name ip";
  return false;
}

static std::unordered_map<uint16_t, std::string> getOpenSSLCipherNames() {
  std::unordered_map<uint16_t, std::string> ret;
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;

  const SSL_METHOD* meth = TLS_server_method();

  if ((ctx = SSL_CTX_new(meth)) == nullptr) {
    return ret;
  }
  SCOPE_EXIT {
    SSL_CTX_free(ctx);
  };

  if ((ssl = SSL_new(ctx)) == nullptr) {
    return ret;
  }
  SCOPE_EXIT {
    SSL_free(ssl);
  };

  STACK_OF(SSL_CIPHER)* sk = SSL_get_ciphers(ssl);
  for (int i = 0; i < sk_SSL_CIPHER_num(sk); i++) {
    const SSL_CIPHER* c = sk_SSL_CIPHER_value(sk, i);
    unsigned long id = SSL_CIPHER_get_id(c);
    // OpenSSL 1.0.2 and prior does weird things such as stuff the SSL/TLS
    // version into the top 16 bits. Let's ignore those for now. This is
    // BoringSSL compatible (their id can be cast as uint16_t)
    uint16_t cipherCode = id & 0xffffUL;
    ret[cipherCode] = SSL_CIPHER_get_name(c);
  }
  return ret;
}

const std::string& OpenSSLUtils::getCipherName(uint16_t cipherCode) {
  // Having this in a hash map saves the binary search inside OpenSSL
  static std::unordered_map<uint16_t, std::string> cipherCodeToName(
      getOpenSSLCipherNames());

  const auto& iter = cipherCodeToName.find(cipherCode);
  if (iter != cipherCodeToName.end()) {
    return iter->second;
  } else {
    static std::string empty;
    return empty;
  }
}

void OpenSSLUtils::setSSLInitialCtx(SSL* ssl, SSL_CTX* ctx) {
  (void)ssl;
  (void)ctx;
}

SSL_CTX* OpenSSLUtils::getSSLInitialCtx(SSL* ssl) {
  (void)ssl;
  return nullptr;
}

BioMethodUniquePtr OpenSSLUtils::newSocketBioMethod() {
  BIO_METHOD* newmeth = nullptr;
  if (!(newmeth = BIO_meth_new(BIO_TYPE_SOCKET, "socket_bio_method"))) {
    return nullptr;
  }
  auto meth = const_cast<BIO_METHOD*>(BIO_s_socket());
  BIO_meth_set_create(newmeth, BIO_meth_get_create(meth));
  BIO_meth_set_destroy(newmeth, BIO_meth_get_destroy(meth));
  BIO_meth_set_ctrl(newmeth, BIO_meth_get_ctrl(meth));
  BIO_meth_set_callback_ctrl(newmeth, BIO_meth_get_callback_ctrl(meth));
  BIO_meth_set_read(newmeth, BIO_meth_get_read(meth));
  BIO_meth_set_write(newmeth, BIO_meth_get_write(meth));
  BIO_meth_set_gets(newmeth, BIO_meth_get_gets(meth));
  BIO_meth_set_puts(newmeth, BIO_meth_get_puts(meth));

  return BioMethodUniquePtr(newmeth);
}

bool OpenSSLUtils::setCustomBioReadMethod(
    BIO_METHOD* bioMeth, int (*meth)(BIO*, char*, int)) {
  bool ret = false;
  ret = (BIO_meth_set_read(bioMeth, meth) == 1);
  return ret;
}

bool OpenSSLUtils::setCustomBioWriteMethod(
    BIO_METHOD* bioMeth, int (*meth)(BIO*, const char*, int)) {
  bool ret = false;
  ret = (BIO_meth_set_write(bioMeth, meth) == 1);
  return ret;
}

int OpenSSLUtils::getBioShouldRetryWrite(int r) {
  int ret = 0;
#ifdef OPENSSL_IS_BORINGSSL
  ret = boringssl_bio_fd_should_retry(r);
#else
  ret = BIO_sock_should_retry(r);
#endif
  return ret;
}

void OpenSSLUtils::setBioAppData(BIO* b, void* ptr) {
#ifdef OPENSSL_IS_BORINGSSL
  BIO_set_callback_arg(b, static_cast<char*>(ptr));
#else
  BIO_set_app_data(b, ptr);
#endif
}

void* OpenSSLUtils::getBioAppData(BIO* b) {
#ifdef OPENSSL_IS_BORINGSSL
  return BIO_get_callback_arg(b);
#else
  return BIO_get_app_data(b);
#endif
}

NetworkSocket OpenSSLUtils::getBioFd(BIO* b) {
  auto ret = BIO_get_fd(b, nullptr);
#ifdef _WIN32
  return NetworkSocket((SOCKET)ret);
#else
  return NetworkSocket(ret);
#endif
}

void OpenSSLUtils::setBioFd(BIO* b, NetworkSocket fd, int flags) {
#ifdef _WIN32
  // Internally OpenSSL uses this as an int for reasons completely
  // beyond any form of sanity, so we do the cast ourselves to avoid
  // the warnings that would be generated.
  int sock = int(fd.data);
#else
  int sock = fd.toFd();
#endif
  BIO_set_fd(b, sock, flags);
}

std::string OpenSSLUtils::getCommonName(X509* x509) {
  if (x509 == nullptr) {
    return "";
  }
  X509_NAME* subject = X509_get_subject_name(x509);
  char buf[ub_common_name + 1];
  int length =
      X509_NAME_get_text_by_NID(subject, NID_commonName, buf, sizeof(buf));
  if (length == -1) {
    // no CN
    return "";
  }
  // length tells us where the name ends
  return std::string(buf, length);
}

std::string OpenSSLUtils::encodeALPNString(
    const std::vector<std::string>& supportedProtocols) {
  unsigned int length = 0;
  for (const auto& proto : supportedProtocols) {
    if (proto.size() > std::numeric_limits<uint8_t>::max()) {
      throw std::range_error("ALPN protocol string exceeds maximum length");
    }
    length += proto.size() + 1;
  }

  std::string encodedALPN;
  encodedALPN.reserve(length);

  for (const auto& proto : supportedProtocols) {
    encodedALPN.append(1, static_cast<char>(proto.size()));
    encodedALPN.append(proto);
  }
  return encodedALPN;
}

/**
 * Deserializes PEM encoded X509 objects from the supplied source BIO, invoking
 * a callback for each X509, until the BIO is exhausted or until we were unable
 * to read an X509.
 */
template <class Callback>
static void forEachX509(BIO* source, Callback cb) {
  while (true) {
    ssl::X509UniquePtr x509(
        PEM_read_bio_X509(source, nullptr, nullptr, nullptr));
    if (x509 == nullptr) {
      ERR_clear_error();
      break;
    }
    cb(std::move(x509));
  }
}

static std::vector<X509NameUniquePtr> getSubjectNamesFromBIO(BIO* b) {
  std::vector<X509NameUniquePtr> ret;
  forEachX509(b, [&](auto&& name) {
    // X509_get_subject_name borrows the X509_NAME, so we must dup it.
    ret.push_back(
        X509NameUniquePtr(X509_NAME_dup(X509_get_subject_name(name.get()))));
  });
  return ret;
}

std::vector<X509NameUniquePtr> OpenSSLUtils::subjectNamesInPEMFile(
    const char* filename) {
  BioUniquePtr bio(BIO_new_file(filename, "r"));
  if (!bio) {
    throw std::runtime_error(
        "OpenSSLUtils::subjectNamesInPEMFile: failed to open file");
  }
  return getSubjectNamesFromBIO(bio.get());
}

std::vector<X509NameUniquePtr> OpenSSLUtils::subjectNamesInPEMBuffer(
    folly::ByteRange buffer) {
  BioUniquePtr bio(BIO_new_mem_buf(buffer.data(), buffer.size()));
  if (!bio) {
    throw std::runtime_error(
        "OpenSSLUtils::subjectNamesInPEMBuffer: failed to create BIO");
  }
  return getSubjectNamesFromBIO(bio.get());
}

} // namespace ssl
} // namespace folly

namespace {
#ifdef OPENSSL_IS_BORINGSSL

static int boringssl_bio_fd_non_fatal_error(int err) {
  if (
#ifdef EWOULDBLOCK
      err == EWOULDBLOCK ||
#endif
#ifdef WSAEWOULDBLOCK
      err == WSAEWOULDBLOCK ||
#endif
#ifdef ENOTCONN
      err == ENOTCONN ||
#endif
#ifdef EINTR
      err == EINTR ||
#endif
#ifdef EAGAIN
      err == EAGAIN ||
#endif
#ifdef EPROTO
      err == EPROTO ||
#endif
#ifdef EINPROGRESS
      err == EINPROGRESS ||
#endif
#ifdef EALREADY
      err == EALREADY ||
#endif
      0) {
    return 1;
  }
  return 0;
}

#if defined(OPENSSL_WINDOWS)

int boringssl_bio_fd_should_retry(int i) {
  if (i == -1) {
    return boringssl_bio_fd_non_fatal_error((int)GetLastError());
  }
  return 0;
}

#else // !OPENSSL_WINDOWS

int boringssl_bio_fd_should_retry(int i) {
  if (i == -1) {
    return boringssl_bio_fd_non_fatal_error(errno);
  }
  return 0;
}
#endif // OPENSSL_WINDOWS

#endif // OEPNSSL_IS_BORINGSSL

} // namespace
