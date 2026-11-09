#include "boolean/query.hpp"
#include "text/unicode.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace boolean_search {
namespace {
struct Node {
    char kind='t';
    std::string term;
    std::unique_ptr<Node> a,b;
};
using Tree=std::unique_ptr<Node>;
Tree binary(char kind,Tree a,Tree b={}) {
    auto n=std::make_unique<Node>(); n->kind=kind; n->a=std::move(a); n->b=std::move(b); return n;
}
struct Token { std::string text; std::size_t position; };
class Parser {
    std::vector<Token> tokens;
    std::size_t current=0, depth=0, nodes=0;
    const Normalize& normalize;
    [[noreturn]] void error(const std::string& reason) const {
        throw std::invalid_argument(reason+" at byte "+std::to_string(tokens[current].position));
    }
    bool take(const std::string& value) {
        if (tokens[current].text!=value) return false;
        ++current; return true;
    }
    Tree term() {
        if (++nodes>512) error("Too many query nodes");
        if (++depth>64) error("Query nesting exceeds 64");
        Tree result;
        if (take("NOT")) result=binary('!',term());
        else if (take("(")) {
            result=disjunction();
            if (!take(")")) error("Expected closing parenthesis");
        } else {
            const auto token=tokens[current];
            if (token.text.empty() || token.text==")" || token.text=="AND" || token.text=="OR") error("Expected operand");
            ++current;
            const auto expanded=normalize(token.text);
            if (expanded.empty()) throw std::invalid_argument("No searchable tokens at byte "+std::to_string(token.position));
            for (auto alternatives:expanded) {
                std::sort(alternatives.begin(),alternatives.end());
                alternatives.erase(std::unique(alternatives.begin(),alternatives.end()),alternatives.end());
                if (alternatives.empty()) error("Empty term expansion");
                Tree group;
                for (const auto& word:alternatives) {
                    if (++nodes>512 || word.empty()) error("Too many or empty query terms");
                    text::validate_utf8(word);
                    auto leaf=std::make_unique<Node>(); leaf->term=word;
                    group=group ? binary('|',std::move(group),std::move(leaf)) : std::move(leaf);
                }
                result=result ? binary('&',std::move(result),std::move(group)) : std::move(group);
            }
        }
        --depth; return result;
    }
    Tree conjunction() {
        auto left=term();
        while (true) {
            if (take("AND")) left=binary('&',std::move(left),term());
            else if (!tokens[current].text.empty() && tokens[current].text!="OR" && tokens[current].text!=")")
                left=binary('&',std::move(left),term());
            else return left;
        }
    }
    Tree disjunction() {
        auto left=conjunction();
        while (take("OR")) left=binary('|',std::move(left),conjunction());
        return left;
    }
public:
    Parser(const std::string& query,const Normalize& normalizer) : normalize(normalizer) {
        if (query.size()>16384) throw std::invalid_argument("Query exceeds 16384 bytes");
        text::validate_utf8(query);
        for (std::size_t i=0;i<query.size();) {
            const auto c=static_cast<unsigned char>(query[i]);
            if (std::isspace(c)) { ++i; continue; }
            const auto start=i;
            if (query[i]=='(' || query[i]==')') ++i;
            else {
                while (i<query.size() && !std::isspace(static_cast<unsigned char>(query[i])) && query[i]!='(' && query[i]!=')') {
                    if (query[i]=='"' || query[i]=='\'' || query[i]=='*' || query[i]=='?' || query[i]=='&' || query[i]=='|' || query[i]=='!')
                        throw std::invalid_argument("Unsupported query character at byte "+std::to_string(i));
                    ++i;
                }
            }
            tokens.push_back({query.substr(start,i-start),start});
            if (tokens.size()>256) throw std::invalid_argument("Query exceeds 256 lexical tokens");
        }
        tokens.push_back({"",query.size()});
    }
    Tree parse() {
        auto result=disjunction();
        if (!tokens[current].text.empty()) error("Unexpected token");
        return result;
    }
};
std::uint64_t estimate(const Index& index,const Node& n) {
    if (n.kind=='t') return index.frequency(n.term);
    if (n.kind=='!') return index.document_count();
    const auto a=estimate(index,*n.a), b=estimate(index,*n.b);
    return n.kind=='&' ? std::min(a,b) : std::min(index.document_count(),a+b);
}
Stream compile(const Index& index,const Node& n) {
    if (n.kind=='t') return index.lookup(n.term);
    if (n.kind=='!') return difference(universe(index.document_count()),compile(index,*n.a));
    if (n.kind=='|') return unite(compile(index,*n.a),compile(index,*n.b));
    if (n.b->kind=='!') return difference(compile(index,*n.a),compile(index,*n.b->a));
    if (n.a->kind=='!') return difference(compile(index,*n.b),compile(index,*n.a->a));
    if (estimate(index,*n.a)>estimate(index,*n.b)) return intersect(compile(index,*n.b),compile(index,*n.a));
    return intersect(compile(index,*n.a),compile(index,*n.b));
}
}
SearchResult search(const Index& index,const std::string& query,const Normalize& normalize,
                    std::size_t limit,std::uint64_t offset,const std::function<void()>& check) {
    const auto tree=Parser(query,normalize).parse();
    auto stream=compile(index,*tree);
    SearchResult result;
    for (;stream->value()!=end_id;stream->next()) {
        if ((result.total%4096)==0 && check) check();
        if (result.total>=offset && result.documents.size()<limit)
            result.documents.push_back(static_cast<DocId>(stream->value()));
        ++result.total;
    }
    return result;
}
}
