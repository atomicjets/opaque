#include <openssl/pem.h>
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <streambuf>

#include "ecp.h"
#include "ias_ra.h"
#include "iasrequest.h"
#include "crypto.h"
#include "base64.h"
#include "json.hpp"

#include <openenclave/host.h>
#include <openenclave/host_verify.h>

#include "ServiceProvider.h"

// Your 16-byte Service Provider ID (SPID), assigned by Intel.
const uint8_t spid[] = {0xA4,0x62,0x09,0x2E,0x1B,0x59,0x26,0xDF,0x44,0x69,0xD5,0x61,0xE2,0x54,0xB0,0x1E};

// The EPID security policy you chose (linkable -> true, unlinkable -> false).
const bool linkable_signature = false;

ServiceProvider service_provider(
  std::string(reinterpret_cast<const char *>(spid), sizeof(spid)),
  linkable_signature,
  // Whether to use the production IAS URL rather than the testing URL.
  false);

void lc_check(lc_status_t ret) {
  if (ret != LC_SUCCESS) {
    std::string error;
    switch (ret) {
    case LC_ERROR_UNEXPECTED:
      error = "Unexpected error";
      break;
    case LC_ERROR_INVALID_PARAMETER:
      error = "Invalid parameter";
      break;
    case LC_ERROR_OUT_OF_MEMORY:
      error = "Out of memory";
      break;
    default:
      error = "Unknown error";
    }

    throw std::runtime_error(
      std::string("Service provider crypto failure: ")
      + error);
  }
}

void ServiceProvider::load_private_key(const std::string &filename) {
  FILE *private_key_file = fopen(filename.c_str(), "r");
  if (private_key_file == nullptr) {
    throw std::runtime_error(
      std::string("Error: Private key file '")
      + filename
      + std::string("' does not exist. Set $PRIVATE_KEY_PATH to the file generated by "
                    "openssl ecparam -genkey, probably called ${OPAQUE_HOME}/private_key.pem."));
  }

  EVP_PKEY *pkey = PEM_read_PrivateKey(private_key_file, NULL, NULL, NULL);
  if (pkey == nullptr) {
    throw std::runtime_error(
      "Unable to read private key from '"
      + filename
      + std::string("'."));
  }

  EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(pkey);
  if (!ec_key) {
    throw std::runtime_error("EVP_PKEY_get1_EC_KEY failed.");
  }

  const EC_GROUP *group = EC_KEY_get0_group(ec_key);
  const EC_POINT *point = EC_KEY_get0_public_key(ec_key);

  BIGNUM *x_ec = BN_new();
  BIGNUM *y_ec = BN_new();
  if (EC_POINT_get_affine_coordinates_GFp(group, point, x_ec, y_ec, nullptr) == 0) {
    throw std::runtime_error("EC_POINT_get_affine_coordinates_GFp failed.");
  }

  const BIGNUM *priv_bn = EC_KEY_get0_private_key(ec_key);

  // Store the public and private keys in binary format
  std::unique_ptr<uint8_t> x_(new uint8_t[LC_ECP256_KEY_SIZE]);
  std::unique_ptr<uint8_t> y_(new uint8_t[LC_ECP256_KEY_SIZE]);
  std::unique_ptr<uint8_t> r_(new uint8_t[LC_ECP256_KEY_SIZE]);

  std::unique_ptr<uint8_t> x(new uint8_t[LC_ECP256_KEY_SIZE]);
  std::unique_ptr<uint8_t> y(new uint8_t[LC_ECP256_KEY_SIZE]);
  std::unique_ptr<uint8_t> r(new uint8_t[LC_ECP256_KEY_SIZE]);

  BN_bn2bin(x_ec, x_.get());
  BN_bn2bin(y_ec, y_.get());
  BN_bn2bin(priv_bn, r_.get());

  // reverse x_, y_, r_
  for (uint32_t i = 0; i < LC_ECP256_KEY_SIZE; i++) {
    x.get()[i] = x_.get()[LC_ECP256_KEY_SIZE-i-1];
    y.get()[i] = y_.get()[LC_ECP256_KEY_SIZE-i-1];
    r.get()[i] = r_.get()[LC_ECP256_KEY_SIZE-i-1];
  }

  // Store public and private keys
  memcpy(sp_pub_key.gx, x.get(), LC_ECP256_KEY_SIZE);
  memcpy(sp_pub_key.gy, y.get(), LC_ECP256_KEY_SIZE);
  memcpy(sp_priv_key.r, r.get(), LC_ECP256_KEY_SIZE);

  // Clean up
  BN_free(x_ec);
  BN_free(y_ec);
  EC_KEY_free(ec_key);
  EVP_PKEY_free(pkey);
}

void ServiceProvider::set_shared_key(const uint8_t *shared_key) {
  memcpy(this->shared_key, shared_key, LC_AESGCM_KEY_SIZE);
}

// This function for testing purposes
// void ServiceProvider::set_test_key(const uint8_t *shared_key) {
//   memcpy(this->test_key, shared_key, LC_AESGCM_KEY_SIZE);
// }

void ServiceProvider::set_user_cert(const std::string user_cert) {
  memcpy(this->user_cert, user_cert.c_str(), user_cert.length() + 1);
  // this->user_cert = user_cert.c_str();
  // std::cout << this->user_cert;
}

void ServiceProvider::set_key_share(const uint8_t *key_share) {
  memcpy(this->key_share, key_share, LC_AESGCM_KEY_SIZE);
}

void ServiceProvider::export_public_key_code(const std::string &filename) {
  std::ofstream file(filename.c_str());

  file << "#include \"key.h\"\n";
  file << "const sgx_ec256_public_t g_sp_pub_key = {\n";

  file << "{";
  for (uint32_t i = 0; i < LC_ECP256_KEY_SIZE; ++i) {
    file << "0x" << std::hex << std::setfill('0') << std::setw(4) << int(sp_pub_key.gx[i]);
    if (i < LC_ECP256_KEY_SIZE - 1) {
      file << ", ";
    }
  }
  file << "},\n";

  file << "{";
  for (uint32_t i = 0; i < LC_ECP256_KEY_SIZE; ++i) {
    file << "0x" << std::hex << std::setfill('0') << std::setw(4) << int(sp_pub_key.gy[i]);
    if (i < LC_ECP256_KEY_SIZE - 1) {
      file << ", ";
    }
  }
  file << "}\n";

  file << "};\n";
  file.close();
}

bool verify_mrsigner(char* signing_public_key_buf,
                     size_t signing_public_key_buf_size,
                     uint8_t* signer_id_buf,
                     size_t signer_id_buf_size) {
  
  mbedtls_pk_context ctx;
  mbedtls_pk_type_t pk_type;
  mbedtls_rsa_context* rsa_ctx = NULL;
  uint8_t* modulus = NULL;
  size_t modulus_size = 0;
  int res = 0;
  bool ret = false;
  unsigned char* signer = NULL;


  signer = (unsigned char*)malloc(signer_id_buf_size);
  if (signer == NULL) {
    printf("Out of memory\n");
    goto exit;
  }

  mbedtls_pk_init(&ctx);

  res = mbedtls_pk_parse_public_key(&ctx,
                                    (const unsigned char*)signing_public_key_buf,
                                    signing_public_key_buf_size+1);

  if (res != 0) {
    printf("mbedtls_pk_parse_public_key failed with %d\n", res);
    goto exit;
  }

  pk_type = mbedtls_pk_get_type(&ctx);
  if (pk_type != MBEDTLS_PK_RSA) {
    printf("mbedtls_pk_get_type had incorrect type: %d\n", res);
    goto exit;
  }

  rsa_ctx = mbedtls_pk_rsa(ctx);
  modulus_size = mbedtls_rsa_get_len(rsa_ctx);
  modulus = (uint8_t*)malloc(modulus_size);
  if (modulus == NULL) {
    printf("malloc for modulus failed with size %zu:\n", modulus_size);
    goto exit;
  }

  res = mbedtls_rsa_export_raw(rsa_ctx, modulus, modulus_size, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
  if (res != 0) {
    printf("mbedtls_rsa_export failed with %d\n", res);
    goto exit;
  }

  // Reverse the modulus and compute sha256 on it.
  for (size_t i = 0; i < modulus_size / 2; i++) {
    uint8_t tmp = modulus[i];
    modulus[i] = modulus[modulus_size - 1 - i];
    modulus[modulus_size - 1 - i] = tmp;
  }

  // Calculate the MRSIGNER value which is the SHA256 hash of the
  // little endian representation of the public key modulus. This value
  // is populated by the signer_id sub-field of a parsed oe_report_t's
  // identity field.

  if (lc_compute_sha256(modulus, modulus_size, signer) != 0) {
    goto exit;
  }

  if (memcmp(signer, signer_id_buf, signer_id_buf_size) != 0) {
    printf("mrsigner is not equal!\n");
    for (size_t i = 0; i < signer_id_buf_size; i++) {
      printf("0x%x - 0x%x\n", (uint8_t)signer[i], (uint8_t)signer_id_buf[i]);
    }
    goto exit;
  }

  ret = true;

 exit:
  if (signer)
    free(signer);

  if (modulus != NULL)
    free(modulus);

  mbedtls_pk_free(&ctx);
  return ret;
}

std::unique_ptr<oe_msg2_t> ServiceProvider::process_msg1(oe_msg1_t *msg1,
                                                         uint32_t *msg2_size) {
  
  int ret;
  unsigned char encrypted_sharedkey[OE_SHARED_KEY_CIPHERTEXT_SIZE];
  size_t encrypted_sharedkey_size = sizeof(encrypted_sharedkey);

  unsigned char encrypted_key_share[OE_SHARED_KEY_CIPHERTEXT_SIZE];
  size_t encrypted_key_share_size = sizeof(encrypted_key_share);

  std::unique_ptr<oe_msg2_t> msg2(new oe_msg2_t);
  
  EVP_PKEY* pkey = buffer_to_public_key((char*)msg1->public_key, -1);
  if (pkey == nullptr) {
    throw std::runtime_error("buffer_to_public_key failed.");
  }



#ifdef SIMULATE
  std::cout << "Not running remote attestation because executing in simulation mode" << std::endl;
#else
  std::cout << "Running in hardware mode, verifying remote attestation\n" ;
  
  //verify report
  oe_report_t parsed_report;
  oe_result_t result = OE_FAILURE;
  uint8_t sha256[32];
  
  result = oe_verify_remote_report(msg1->report, msg1->report_size, NULL, 0, &parsed_report);
  if (result != OE_OK) {
    throw std::runtime_error(
                             std::string("oe_verify_remote_report: ")
                             + oe_result_str(result));
  }

  printf("OE report verified\n");

  // mrsigner verification
  // 2) validate the enclave identity's signed_id is the hash of the public
  // signing key that was used to sign an enclave. Check that the enclave was
  // signed by an trusted entity.
  
  // 2a) Read in the public key as a string

  std::string public_key_file = std::string(std::getenv("OPAQUE_HOME"));
  public_key_file.append("/public_key.pub");

  std::ifstream t(public_key_file.c_str());
  std::string public_key;

  t.seekg(0, std::ios::end);
  size_t public_key_size = t.tellg();
  public_key.reserve(public_key_size + 1);
  t.seekg(0, std::ios::beg);

  public_key.assign((std::istreambuf_iterator<char>(t)),
                    std::istreambuf_iterator<char>());
  public_key.replace(public_key_size, 1, "\0");

  if (!verify_mrsigner((char*)public_key.c_str(),
                       public_key.size(),
                       parsed_report.identity.signer_id,
                       sizeof(parsed_report.identity.signer_id))) {
    throw std::runtime_error(std::string("failed: mrsigner not equal!"));
  }

  std::cout << "Signer verification passed\n" ;

  // TODO missing the hash verification step

  // check the enclave's product id and security version
  if (parsed_report.identity.product_id[0] != 1) {
    throw std::runtime_error(std::string("identity.product_id checking failed."));
  }

  if (parsed_report.identity.security_version < 1) {
    throw std::runtime_error(std::string("identity.security_version checking failed."));
  }

  // 3) Validate the report data
  //    The report_data has the hash value of the report data
  if (lc_compute_sha256(msg1->public_key, sizeof(msg1->public_key), sha256) != 0) {
    throw std::runtime_error(std::string("hash validation failed."));
  }

  if (memcmp(parsed_report.report_data, sha256, sizeof(sha256)) != 0) {
    throw std::runtime_error(std::string("SHA256 mismatch."));
  }
  
  std::cout << "remote attestation succeeded." << std::endl;
#endif

  // Encrypt shared key
  ret = public_encrypt(pkey, this->shared_key, LC_AESGCM_KEY_SIZE, encrypted_sharedkey, &encrypted_sharedkey_size);
  if (ret == 0) {
    throw std::runtime_error(std::string("public_encrypt: buffer too small"));
  }
  else if (ret < 0) {
    throw std::runtime_error(std::string("public_encrypt failed"));
  }

  // Encrypt key share
  ret = public_encrypt(pkey, this->key_share, LC_AESGCM_KEY_SIZE, encrypted_key_share, &encrypted_key_share_size);
  if (ret == 0) {
    throw std::runtime_error(std::string("public_encrypt: buffer too small"));
  }
  else if (ret < 0) {
    throw std::runtime_error(std::string("public_encrypt failed"));
  }
 
  // Encrypt test key for testing purposes
  // FIXME: remove this block - it was for testing purposes
  // unsigned char encrypted_test_key[OE_SHARED_KEY_CIPHERTEXT_SIZE];
  // size_t encrypted_test_key_size = sizeof(encrypted_test_key);
  // ret = public_encrypt(pkey, this->test_key, LC_AESGCM_KEY_SIZE, encrypted_test_key, &encrypted_test_key_size);
  // if (ret == 0) {
  //   throw std::runtime_error(std::string("public_encrypt: buffer too small"));
  // }
  // else if (ret < 0) {
  //   throw std::runtime_error(std::string("public_encrypt failed"));
  // }
  // memcpy_s(msg2->test_key_ciphertext, OE_SHARED_KEY_CIPHERTEXT_SIZE, encrypted_test_key, encrypted_test_key_size);
  // FIXME: remove up to here

  // Prepare msg2
  // Copy over shared key ciphertext
  memcpy_s(msg2->shared_key_ciphertext, OE_SHARED_KEY_CIPHERTEXT_SIZE, encrypted_sharedkey, encrypted_sharedkey_size);

  // Copy over key share ciphertext
  memcpy_s(msg2->key_share_ciphertext, OE_SHARED_KEY_CIPHERTEXT_SIZE, encrypted_key_share, encrypted_key_share_size);


  // Copy user certificate to msg2
  size_t cert_len = strlen(this->user_cert) + 1;
  memcpy_s(msg2->user_cert, cert_len, this->user_cert, cert_len);
  msg2->user_cert_len = cert_len;
  *msg2_size = sizeof(msg2->shared_key_ciphertext) + sizeof(msg2->key_share_ciphertext) + sizeof(msg2->user_cert_len) + sizeof(msg2->user_cert);

  // clean up
  EVP_PKEY_free(pkey);

  return msg2;
}
