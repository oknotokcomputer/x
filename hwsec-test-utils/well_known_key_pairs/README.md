# Well-known secrets used for testing.

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/hwsec-test-utils/well_known_key_pairs/README.md
***

In this folder, we maintain the well known key pairs injected for attestation
testing. Below are documents for the respective files of which documents are not
suitable to be maintained in the files themselves.

ca_encryption.pem: Well-known RSA key used to encrypt/decrypt the endorment
certificate.

va_encryption.pem: Well-known RSA key used to encrypt/decrypt the SPKAC result
in VA process.

va_signing.pem: Well-known RSA key used to sign/verify the VA challenge.
