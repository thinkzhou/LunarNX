#pragma once

namespace lunar::ps {

// Select the fastest available Chiaki GHASH implementation before any
// PlayStation session or GMAC context is created.
void initializeChiakiCrypto();

} // namespace lunar::ps
