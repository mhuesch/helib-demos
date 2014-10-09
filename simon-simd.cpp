#include <algorithm>
#include "simon-plaintext.h"
#include "simon-util.h"

long global_nslots;

class ctvec;

ctvec* global_maxint;
ctvec* global_one;
ctvec* global_zero;

class ctvec {
    vector<Ctxt> cts;
    EncryptedArray* ea;
    const FHEPubKey* pubkey;
    int nelems;

public:
    // inp comes in already bitsliced/transposed
    ctvec
    (
        EncryptedArray &inp_ea,
        const FHEPubKey &inp_pubkey,
        vector<vector<long>> inp,
        bool fill = false
    )
    {
        ea = &inp_ea;
        pubkey = &inp_pubkey;
        nelems = inp[0].size();
        for (int i = 0; i < inp.size(); i++) {
            if (fill) {
                pad(inp[i][0]&1, inp[i], global_nslots);
            } else {
                pad(0, inp[i], global_nslots);
            }
            Ctxt c(*pubkey);
            ea->encrypt(c, *pubkey, inp[i]);
            cts.push_back(c);
        }
    }

    Ctxt get (int i) { return cts[i]; }

    void xorWith (ctvec &other) {
        for (int i = 0; i < cts.size(); i++) {
            cts[i].addCtxt(other.get(i));
        }
    }

    void andWith (ctvec &other) {
        for (int i = 0; i < cts.size(); i++) {
            cts[i].multiplyBy(other.get(i));
        }
    }

    void rotateLeft (int n) {
        rotate(cts.begin(), cts.end()-n, cts.end());
    }

    vector<vector<long>> decrypt (const FHESecKey &seckey) {
        vector<vector<long>> res;
        for (int i = 0; i < cts.size(); i++) {
            vector<long> bits (global_nslots);
            ea->decrypt(cts[i], seckey, bits);
            res.push_back(bits);
        }
        return res;
    }
};

struct pt_preblock {
    vector<vector<long>> xs;
    vector<vector<long>> ys;
};

struct heblock {
    ctvec x;
    ctvec y;
};

vector<pt_block> preblockToBlocks (pt_preblock b) {
    vector<pt_block> bs;
    for (int j = 0; j < b.xs[0].size(); j++) {
        vector<long> xbits;
        vector<long> ybits;
        for (int i = 0; i < 32; i++) {
            xbits.push_back(b.xs[i][j]);
            ybits.push_back(b.ys[i][j]);
        }
        uint32_t x = vectorTo32(xbits);
        uint32_t y = vectorTo32(ybits);
        bs.push_back({ x, y });
    }
    return bs;
}

pt_preblock blocksToPreblock (vector<pt_block> bs) {
    vector<vector<long>> xs;
    vector<vector<long>> ys;
    for (int i = 0; i < 32; i++) {
        vector<long> nextx;
        vector<long> nexty;
        for (int j = 0; j < bs.size(); j++) {
            vector<long> x = uint32ToBits(bs[j].x);
            vector<long> y = uint32ToBits(bs[j].y);
            nextx.push_back(x[i]);
            nexty.push_back(y[i]);
        }
        xs.push_back(nextx);
        ys.push_back(nexty);
    }
    return { xs, ys };
}

vector<pt_block> heblockToBlocks (const FHESecKey &k, heblock ct) {
    vector<pt_block> res;
    vector<vector<long>> xs = ct.x.decrypt(k);
    vector<vector<long>> ys = ct.y.decrypt(k);
    return preblockToBlocks({ xs, ys });
}

heblock heEncrypt (EncryptedArray &ea, const FHEPubKey &pubkey, string s) {
    vector<pt_block> pt = strToBlocks(s);
    pt_preblock b = blocksToPreblock(pt);
    ctvec c0 (ea, pubkey, b.xs);
    ctvec c1 (ea, pubkey, b.ys);
    return { c0, c1 };
}

vector<ctvec> heEncrypt (EncryptedArray &ea, const FHEPubKey &pubkey, vector<uint32_t> &k) {
    vector<ctvec> encryptedKey;
    for (int i = 0; i < k.size(); i++) {
        vector<long> bits = uint32ToBits(k[i]);
        vector<vector<long>> trans = transpose(bits);
        ctvec kct (ea, pubkey, trans, true);
        encryptedKey.push_back(kct);
    }
    return encryptedKey;
}

void encRound(ctvec key, heblock &inp) {
    ctvec tmp = inp.x;
    ctvec x0 = inp.x;
    ctvec x1 = inp.x;
    ctvec x2 = inp.x;
    ctvec y = inp.y;
    x0.rotateLeft(1);
    x1.rotateLeft(8);
    x2.rotateLeft(2);
    x0.andWith(x1);
    y.xorWith(x0);
    y.xorWith(x2);
    y.xorWith(key);
    inp.x = y;
    inp.y = tmp;
}

int main(int argc, char **argv)
{
    string inp = "secrets! very secrets!";
    cout << "inp = \"" << inp << "\"" << endl;
    vector<pt_key32> k = pt_genKey();
    pt_expandKey(k);
    printKey(k);

    // initialize helib
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
    FHESecKey seckey(context);
    const FHEPubKey& pubkey = seckey;
    G = context.alMod.getFactorsOverZZ()[0];
    seckey.GenSecKey(w);
    addSome1DMatrices(seckey);
    EncryptedArray ea(context, G);
    long nslots = ea.size();
    cout << "nslots = " << nslots << endl;
    global_nslots = nslots;

    // set up globals
    timer(true);
    cout << "Encrypting constants..." << flush;
    ctvec one (ea, pubkey, transpose(uint32ToBits(1)));
    ctvec zero (ea, pubkey, transpose(uint32ToBits(0)));
    ctvec maxint (ea, pubkey, transpose(uint32ToBits(0xFFFFFFFF)));
    global_maxint = &maxint;
    global_zero = &zero;
    global_one = &one;
    timer();

    // HEencrypt key
    cout << "Encrypting SIMON key..." << flush;
    vector<ctvec> encryptedKey = heEncrypt(ea, pubkey, k);
    timer();

    // HEencrypt input
    cout << "Encrypting inp..." << flush;
    heblock ct = heEncrypt(ea, pubkey, inp);
    timer();

    cout << "Running protocol..." << endl;
    for (int i = 0; i < T; i++) {
        cout << "Round " << i+1 << "/" << T << "..." << flush;
        encRound(encryptedKey[i], ct);
        timer();

        // check intermediate result for noise
        cout << "decrypting..." << flush;
        vector<pt_block> bs = heblockToBlocks(seckey, ct);
        timer();
        cout << "result: \"" << pt_simonDec(k, bs, i+1) << "\" " << endl;
    }

    timer(true);
    cout << "Decrypting result..." << flush;
    vector<pt_block> bs = heblockToBlocks(seckey, ct);
    timer();
    cout << "result: \"" << pt_simonDec(k, bs, T) << "\"" << endl;
    return 0;
}
