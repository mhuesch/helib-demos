#include "FHE.h"
#include <string>
#include <stdio.h>
#include <ctime>
#include "EncryptedArray.h"
#include "permutations.h"
#include <NTL/lzz_pXFactoring.h>
#include <fstream>
#include <sstream>
#include <sys/time.h>

#include <math.h>
#include <stdint.h>
#include <bitset>
#include <tuple>
#include <stdexcept>
#include <algorithm>

std::bitset<62> z0(0b11111010001001010110000111001101111101000100101011000011100110);
std::bitset<62> z1(0b10001110111110010011000010110101000111011111001001100001011010);
std::bitset<62> z2(0b10101111011100000011010010011000101000010001111110010110110011);
std::bitset<62> z3(0b11011011101011000110010111100000010010001010011100110100001111);
std::bitset<62> z4(0b11010001111001101011011000100000010111000011001010010011101111);
bitset<62> z[5] = { z0, z1, z2, z3, z4 };

typedef uint32_t key32;
typedef vector<key32> key;

typedef struct {
    uint32_t x;
    uint32_t y;
} block;

typedef vector<block> ciphertext;

const int m = 4;
const int j = 3;
//const int T = 44;
const int T = 4;


EncryptedArray* global_ea;
Ctxt* global_maxint;
Ctxt* global_one;
Ctxt* global_zero;
long* global_nslots;
const FHEPubKey* global_pubkey;

// Plaintext SIMON

uint32_t rotateLeft(uint32_t x, int n) {
    return x << n | x >> 32 - n;
}

block encRound(key32 k, block inp) {
    block ret = { inp.y ^ (rotateLeft(inp.x,1) & rotateLeft(inp.x,8)) ^ rotateLeft(inp.x,2) ^ k, inp.x };
    return ret;
}

block decRound(key32 k, block inp) {
    block ret = { inp.y, inp.x ^ (rotateLeft(inp.y,1) & rotateLeft(inp.y,8)) ^ rotateLeft(inp.y,2) ^ k };
    return ret;
}

key expandKey(key inp){
    key ks = inp;
    ks.reserve(T-1);
    if (ks.size() != 4) 
        throw std::invalid_argument( "Key must be of length 4" );
    for (int i = m; i < T-1; i++) {
        key32 tmp;
        tmp = rotateLeft(ks[i-1], 3) ^ ks[i-3];
        tmp ^= rotateLeft(tmp, 1);
        tmp ^= 3;
        tmp ^= z[j][i-m % 62];
        tmp ^= ~ks[i-m];
        ks.push_back(tmp);
    }
    return ks;
}


block encrypt(key k, block inp) {
    block res = inp;
    key ks = expandKey(k);
    for (int i = 0; i < ks.size(); i++) {
        res = encRound(ks[i], res);   
    }
    return res;
}

block decrypt(key k, block inp) {
    block res = inp;
    key ks = expandKey(k);
    std::reverse(ks.begin(), ks.end());
    for (int i = 0; i < ks.size(); i++) {
        res = decRound(ks[i], res);   
    }
    return res;
}

// Homomorphic SIMON

void rotateLeft(Ctxt &x, int n) {
    Ctxt other = x;
    global_ea->shift(other, n);
    global_ea->shift(x, -(32 - n));
    x += other;
    x *= *global_maxint; // mask to keep leftmost (nslots-32) bits zeroed out.
}

void encRound(Ctxt key, Ctxt &x, Ctxt &y) {
    Ctxt tmp = x;
    Ctxt x0 = x; 
    Ctxt x1 = x; 
    Ctxt x2 = x; 
    rotateLeft(x0, 1);
    rotateLeft(x1, 8);
    rotateLeft(x2, 2);
    y += x0;
    y += x1;
    y += x2;
    y += key;
    x = y;
    y = tmp;
}

void negate32(Ctxt &x) {
    x += *global_maxint;
    x *= *global_maxint;
}

Ctxt heEncrypt(uint32_t);

void expandKey(vector<Ctxt> &ks) {
    cout << "PROTO: running expandKey...";
    static Ctxt three = heEncrypt(3);
    for (int i = m; i < T-1; i++) {
        cout << " " << i+1 << "/" << T-1;
        Ctxt tmp = ks[i-1];
        rotateLeft(tmp, 3);
        tmp += ks[i-3];
        Ctxt tmp2 = tmp;
        rotateLeft(tmp2, 1);
        tmp += tmp2;
        tmp += three;
        Ctxt theZ(*global_pubkey);
        if (z[j][i-m % 62])
            theZ = *global_one;
        else
            theZ = *global_zero;
        tmp += theZ;
        Ctxt iminusm = ks[i-m];
        negate32(iminusm);
        tmp += iminusm;
        ks.push_back(tmp);
    }
    cout << endl;
}

void encrypt(vector<Ctxt> k, Ctxt x, Ctxt y) {
    expandKey(k);
    for (int i = 0; i < T-1; i++) {
        cout << "PROTO: running encRound " << i+1 << "/" << k.size() << "..." << endl;
        encRound(k[i], x, y);
    }
}

// HELPERS

key genKey() {
    srand(time(NULL));
    key ks;
    for (int i = 0; i < 4; i++) {
        ks.push_back(rand());
    }
    return ks;
}

uint32_t rand32() {
    srand(time(NULL));
    uint32_t x = 0;
    for (int i = 0; i < 32; i++) {
        x |= rand()%2 << i;
    }
    return x;
}

ciphertext simonEnc(string inp, key k) {
    ciphertext blocks;
    for (int i = 0; i < inp.size(); i += 8) {
        uint32_t x = inp[i]   << 24 | inp[i+1] << 16 | inp[i+2] << 8 | inp[i+3];
        uint32_t y = inp[i+4] << 24 | inp[i+5] << 16 | inp[i+6] << 8 | inp[i+7];
        block b = { x, y };
        blocks.push_back(encrypt(k,b));
    }
    return blocks;
}

string simonDec(ciphertext c, key k) {
    string ret;
    for (int i = 0; i < c.size(); i++) {
        block b = decrypt(k, c[i]);
        ret.push_back(b.x >> 24);
        ret.push_back(b.x >> 16 & 255);
        ret.push_back(b.x >> 8  & 255);
        ret.push_back(b.x & 255);
        ret.push_back(b.y >> 24);
        ret.push_back(b.y >> 16 & 255);
        ret.push_back(b.y >> 8  & 255);
        ret.push_back(b.y & 255);
    }
    return ret;
}

//int main(int argc, char **argv) {
    //key k = genKey();
    //string inp = "hello world, what is up in the neighborhood?";
    //cout << "inp = " << inp << endl;
    //ciphertext c = simonEnc(inp, k);
    //string r = simonDec(c,k);
    //cout << "decrypted = " << r << endl;
    //return 0;
//}

void addCharBits(char c, vector<long> *v) {
    for (int i = 0; i < 8; i++) {
        v->push_back((c & (1 << i)) >> i);
    }
}

void addUint32Bits(uint32_t n, vector<long> *v) {
    for (int i = 0; i < 32; i++) {
        v->push_back((n & (1 << i)) >> i);
    }
}


char charFromBits(vector<long> inp) {
    char ret;
    for (int i = 0; i < 8; i++) {
        ret |= inp[i] << i;
    }
    return ret;
}

uint32_t uint32FromBits(vector<long> inp) {
    uint32_t ret = 0;
    for (int i = 0; i < 32; i++) {
        ret |= inp[i] << i;
    }
    return ret;
}

void pad(vector<long> *inp, int nslots) {
    inp->reserve(nslots);
    for (int i = inp->size(); i < nslots; i++) {
        inp->push_back(0);
    }
}

// returns a vector of an even number of padded vectors with 32 bits of
// information in them. each long is actually just 0 or 1.
vector<vector<long>> vectorizeStr(string inp, int nslots) {
    vector<vector<long>> ret;   
    ret.reserve(inp.size()/4);
    for (int i = 0; i < inp.size(); i+=4) {
        vector<long> c;
        addCharBits(inp[i+0], &c);
        addCharBits(inp[i+1], &c);
        addCharBits(inp[i+2], &c);
        addCharBits(inp[i+3], &c);
        pad(&c, nslots);
        ret.push_back(c);
    }
    if (ret.size()%2) {
        vector<long> c;
        pad(&c, nslots);
        ret.push_back(c);
    }
    return ret;
}

// takes a vector of vectors where each long is 0 or 1. turns it into a string.
string devectorizeStr(vector<vector<long>> inp){
    string ret;
    for (int i = 0; i < inp.size(); i++) {
        vector<long> v0(&inp[i][ 0], &inp[i][ 8]);
        vector<long> v1(&inp[i][ 8], &inp[i][16]);
        vector<long> v2(&inp[i][16], &inp[i][24]);
        vector<long> v3(&inp[i][24], &inp[i][32]);
        ret.push_back(charFromBits(v0));
        ret.push_back(charFromBits(v1));
        ret.push_back(charFromBits(v2));
        ret.push_back(charFromBits(v3));
    }
    return ret;
}

// turns a SIMON key (vector<uint32_t>) into a vector<vector<long>> where each
// long is simply 0 or 1.
vector<vector<long>> vectorizeKey(vector<uint32_t> key, int nslots) {
    vector<vector<long>> ret;
    for (int i = 0; i < key.size(); i++) {
        vector<long> k;
        addUint32Bits(key[i], &k);
        pad(&k, nslots);
        ret.push_back(k);
    }
    return ret;
}

vector<uint32_t> devectorizeKey(vector<vector<long>> inp) {
    vector<uint32_t> ret;
    for (int i = 0; i < inp.size(); i++) {
        ret.push_back(uint32FromBits(inp[i]));
    }
    return ret;
}

void printKey(key k) {
    cout << "key = ";
    for (int i = 0; i < k.size(); i++) {
        printf("%X ", k[i]);
    }
    cout << endl;
}

vector<block> vectorsToBlocks(vector<vector<long>> inp) { 
    vector<block> res;
    for (int i = 0; i < inp.size(); i+=2) {
        block b = { uint32FromBits(inp[i]), uint32FromBits(inp[i+1]) };
        res.push_back(b);
    }
    return res;
}

string blocksToStr(vector<block> bs) {
    vector<string> vec;
    stringstream ss;
    for (int i = 0; i < bs.size(); i++) {
        ss << std::hex << std::uppercase << bs[i].x << bs[i].y;
    }
    return ss.str();
}

// lifts a uint32_t into the HE monad
Ctxt heEncrypt(uint32_t x) {
    vector<long> vec;
    addUint32Bits(x, &vec);
    pad(&vec, global_ea->size());
    Ctxt c(*global_pubkey);
    global_ea->encrypt(c, c.getPubKey(), vec);
    return c;
}

int main(int argc, char **argv)
{
    string inp = "cats";
    cout << "inp = \"" << inp << "\"" << endl;
    key k = genKey();
    printKey(k);

    long m=0, p=2, r=1;
    long L=16;
    long c=3;
    long w=64;
    long d=0;
    long security = 128;
    ZZX G;
    cout << "Finding m..." << endl;
    m = FindM(security,L,c,p,d,0,0);
    cout << "Generating context..." << endl;
    FHEcontext context(m, p, r);
    cout << "Building mod-chain..." << endl;
    buildModChain(context, L, c);
    cout << "Generating keys..." << endl;
    FHESecKey secretKey(context);
    const FHEPubKey& publicKey = secretKey;
    G = context.alMod.getFactorsOverZZ()[0];
    secretKey.GenSecKey(w);
    addSome1DMatrices(secretKey);

    EncryptedArray ea(context, G);
    long nslots = ea.size();

    // set up globals
    global_nslots = &nslots;
    global_ea     = &ea;
    global_pubkey = &publicKey;

    // heEncrypt uses globals
    Ctxt maxint   = heEncrypt(0xFFFFFFFF);
    Ctxt zero     = heEncrypt(0);
    Ctxt one      = heEncrypt(1);
    global_maxint = &maxint;
    global_zero   = &zero;
    global_one    = &one;

    // HEencrypt key
    cout << "Encrypting key..." << endl;
    vector<vector<long>> kbits = vectorizeKey(k, nslots);
    vector<Ctxt> encryptedKey; 
    for (int i = 0; i < kbits.size(); i++) {
        Ctxt kct(publicKey);
        ea.encrypt(kct, publicKey, kbits[i]);
        encryptedKey.push_back(kct);
    }

    // HEencrypt input
    cout << "Encrypting inp..." << endl;
    vector<vector<long>> pt = vectorizeStr(inp, nslots);
    vector<Ctxt> cts;
    for (int i = 0; i < pt.size(); i++) {
        Ctxt c(publicKey);
        ea.encrypt(c, publicKey, pt[i]);
        cts.push_back(c);
    }
    cout << "Running protocol..." << endl;

    // cts contains HEenc(vectorized(inp)), 
    // encryptedKey contains HEenc(vectorized(k))

    for (int i = 0; i < cts.size(); i+=2) {
        encrypt(encryptedKey, cts[i], cts[i+1]);
    }

    // now cts contains HEenc(simonEnc(k, inp))

    cout << "Decrypting result..." << endl;
    for (int i = 0; i < pt.size(); i++) {
        ea.decrypt(cts[i], secretKey, pt[i]);
    }

    // pt = vectorized(simonEnc(inp)), now check that it equals what it should.
    
    //TODO: Test encRound

    cout << "he enc(inp) = " << blocksToStr(vectorsToBlocks(pt)) << endl;
    cout << "he result   = " << simonDec(vectorsToBlocks(pt), k) << endl;
    cout << "pt enc(inp) = " << blocksToStr(simonEnc(inp, k)) << endl;
    cout << "pt result   = " << simonDec(simonEnc(inp, k), k) << endl;

    return 0;
}
