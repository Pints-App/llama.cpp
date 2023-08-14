#define LLAMA_API_CPP // TODO: eliminate me
#include "llama.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <codecvt>
#include <map>
#include <vector>

static std::string vocab_type(llama_context* ctx) {
    return llama_n_vocab(ctx) == 32000 ? "spm": "bpe";
}

static std::string escape_whitespace(const std::string& text) {
    std::string result;
    bool escaping = false;
    result += "\xe2\x96\x81";
    for (size_t offs = 0; offs < text.length(); ++offs) {
        if (text[offs] == ' ') {
            if (!escaping) {
                result += "\xe2\x96\x81";
                escaping = true;
            }
        }
        else {
            escaping = false;
            result += text[offs];
        }
    }
    return result;
}

static std::string unescape_whitespace(llama_context* ctx, const std::vector<llama_token>& tokens) {
    std::string result;
    for (int i = 0; i < tokens.size(); ++i) {
        result += llama_token_to_str(ctx, tokens[i]);
    }
    return result;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <vocab-file>\n", argv[0]);
        return 1;
    }

    const std::string fname = argv[1];

    fprintf(stderr, "%s : reading vocab from: '%s'\n", __func__, fname.c_str());

    llama_model * model;
    llama_context * ctx;

    llama_backend_init(false);

    // load the vocab
    {
        auto lparams = llama_context_default_params();

        lparams.vocab_only = true;

        model = llama_load_model_from_file(fname.c_str(), lparams);

        if (model == NULL) {
            fprintf(stderr, "%s: error: failed to load vocab '%s'\n", __func__, fname.c_str());
            return 1;
        }

        ctx = llama_new_context_with_model(model, lparams);

        if (ctx == NULL) {
            fprintf(stderr, "%s: error: failed to load vocab '%s'\n", __func__, fname.c_str());
            llama_free_model(model);
            return 1;
        }
    }

    const int n_vocab = llama_n_vocab(ctx);

    for (int i = 0; i < n_vocab; ++i) {
        std::string forward = llama_token_to_str_bpe(ctx, i);
        std::vector<llama_token> tokens = llama_tokenize_bpe(ctx, forward, false);
        if (tokens.size() == 1) {
            if (i != tokens[0]) {
                std::string backward = llama_token_to_str(ctx, tokens[0]);
                fprintf(stderr, "%s : error: token %d is string %s but bpe returns token %d %s\n", 
                    __func__, i, llama_token_to_str(ctx, i).c_str(), tokens[0], backward.c_str());
                return 2;
            }
        } else {
            if ((vocab_type(ctx) == "spm" && i <= 258) || 
                (vocab_type(ctx) == "bpe" && (i == 0 || i >= 100000))) {
                fprintf(stderr, "%s : info: token %d is string %s and bpe returns tokens %s\n", 
                    __func__, i, llama_token_to_str(ctx, i).c_str(), unescape_whitespace(ctx, tokens).c_str());
            } else {
                fprintf(stderr, "%s : error: token %d is string %s but bpe returns tokens %s\n", 
                    __func__, i, llama_token_to_str(ctx, i).c_str(), unescape_whitespace(ctx, tokens).c_str());
                return 2;
            }
        }
    }

    std::wstring_convert<typename std::codecvt_utf8<wchar_t>, wchar_t> converter;
    for (wchar_t ch = 0x0000; ch < 0xffff; ++ch) {
        std::wstring wstr(1, ch);
        std::string str = converter.to_bytes(wstr);
        std::vector<llama_token> tokens = llama_tokenize(ctx, escape_whitespace(str).c_str(), false);
        if (tokens.size() == 1) {
            fprintf(stderr, "%s : info: %s tokenized to %d \n", 
                __func__, str.c_str(), tokens[0]);
        }
    }

    llama_free_model(model);
    llama_free(ctx);

    llama_backend_free();

    return 0;
}
