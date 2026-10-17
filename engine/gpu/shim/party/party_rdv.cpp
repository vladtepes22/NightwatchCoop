// SPDX-License-Identifier: GPL-3.0-or-later
#include "party/party_rdv.h"

#include <algorithm>
#include <cstring>

#include "party/party_addr.h"
#include "party/party_crypto.h"
#include "party/party_sock.h"

namespace party::rdv {

Id info_hash(const std::uint8_t* party_key, std::size_t n) {
    static const char kLabel[] = "bbport rendezvous v1";
    std::vector<std::uint8_t> buf(kLabel, kLabel + sizeof(kLabel) - 1);
    buf.insert(buf.end(), party_key, party_key + n);
    const crypto::Key h = crypto::blake2b256(buf.data(), buf.size());
    Id id{};
    std::memcpy(id.data(), h.data(), id.size());
    return id;
}

// ---- bencode ----

const BNode* BNode::get(const char* key) const {
    if (kind != Dict) return nullptr;
    for (const auto& [k, v] : dict)
        if (k == key) return &v;
    return nullptr;
}
BNode BNode::str(std::string v) {
    BNode b;
    b.kind = Str;
    b.s = std::move(v);
    return b;
}
BNode BNode::num(std::int64_t v) {
    BNode b;
    b.kind = Int;
    b.i = v;
    return b;
}

namespace {
struct Dec {
    const std::uint8_t* d;
    std::size_t n, pos = 0;
    int items = 0;
    bool value(BNode* out, int depth) {
        if (depth > 64 || ++items > 4096 || pos >= n) return false;
        const char c = static_cast<char>(d[pos]);
        if (c == 'i') {
            ++pos;
            bool neg = false;
            if (pos < n && d[pos] == '-') {
                neg = true;
                ++pos;
            }
            std::int64_t v = 0;
            int digits = 0;
            while (pos < n && d[pos] >= '0' && d[pos] <= '9') {
                if (++digits > 18) return false;
                v = v * 10 + (d[pos++] - '0');
            }
            if (!digits || pos >= n || d[pos] != 'e') return false;
            ++pos;
            out->kind = BNode::Int;
            out->i = neg ? -v : v;
            return true;
        }
        if (c >= '0' && c <= '9') {
            std::size_t len = 0;
            int digits = 0;
            while (pos < n && d[pos] >= '0' && d[pos] <= '9') {
                if (++digits > 6) return false;
                len = len * 10 + (d[pos++] - '0');
            }
            if (pos >= n || d[pos] != ':') return false;
            ++pos;
            if (len > n - pos) return false;
            out->kind = BNode::Str;
            out->s.assign(reinterpret_cast<const char*>(d + pos), len);
            pos += len;
            return true;
        }
        if (c == 'l' || c == 'd') {
            ++pos;
            out->kind = c == 'l' ? BNode::List : BNode::Dict;
            while (pos < n && d[pos] != 'e') {
                if (c == 'l') {
                    out->list.emplace_back();
                    if (!value(&out->list.back(), depth + 1)) return false;
                } else {
                    BNode k;
                    if (!value(&k, depth + 1) || k.kind != BNode::Str) return false;
                    out->dict.emplace_back(std::move(k.s), BNode{});
                    if (!value(&out->dict.back().second, depth + 1)) return false;
                }
            }
            if (pos >= n) return false;
            ++pos;
            return true;
        }
        return false;
    }
};

void enc(const BNode& v, std::string& o) {
    switch (v.kind) {
    case BNode::Int: o += "i" + std::to_string(v.i) + "e"; break;
    case BNode::Str:
        o += std::to_string(v.s.size()) + ":";
        o += v.s;
        break;
    case BNode::List:
        o += "l";
        for (const BNode& x : v.list) enc(x, o);
        o += "e";
        break;
    case BNode::Dict: {
        std::vector<const std::pair<std::string, BNode>*> items;
        for (const auto& kv : v.dict) items.push_back(&kv);
        std::sort(items.begin(), items.end(), [](auto* a, auto* b) { return a->first < b->first; });
        o += "d";
        for (auto* kv : items) {
            o += std::to_string(kv->first.size()) + ":" + kv->first;
            enc(kv->second, o);
        }
        o += "e";
        break;
    }
    }
}

std::string id_str(const Id& id) { return std::string(reinterpret_cast<const char*>(id.data()), id.size()); }

BNode query(const std::string& tid, const char* q, BNode args) {
    BNode m;
    m.kind = BNode::Dict;
    m.dict.emplace_back("t", BNode::str(tid));
    m.dict.emplace_back("y", BNode::str("q"));
    m.dict.emplace_back("q", BNode::str(q));
    m.dict.emplace_back("a", std::move(args));
    m.dict.emplace_back("ro", BNode::num(1));  // BEP 43: a read-only node, keep us out of tables
    return m;
}
}  // namespace

bool bdecode(const std::uint8_t* d, std::size_t n, BNode* out) {
    Dec dec{d, n};
    *out = BNode{};
    return dec.value(out, 0) && dec.pos == n;
}

std::string bencode(const BNode& v) {
    std::string o;
    enc(v, o);
    return o;
}

std::string make_get_peers(const std::string& tid, const Id& self, const Id& target) {
    BNode a;
    a.kind = BNode::Dict;
    a.dict.emplace_back("id", BNode::str(id_str(self)));
    a.dict.emplace_back("info_hash", BNode::str(id_str(target)));
    return bencode(query(tid, "get_peers", std::move(a)));
}

std::string make_find_node(const std::string& tid, const Id& self, const Id& target) {
    BNode a;
    a.kind = BNode::Dict;
    a.dict.emplace_back("id", BNode::str(id_str(self)));
    a.dict.emplace_back("target", BNode::str(id_str(target)));
    return bencode(query(tid, "find_node", std::move(a)));
}

std::string make_announce(const std::string& tid, const Id& self, const Id& target, std::uint16_t port,
                          const std::string& token) {
    BNode a;
    a.kind = BNode::Dict;
    a.dict.emplace_back("id", BNode::str(id_str(self)));
    a.dict.emplace_back("info_hash", BNode::str(id_str(target)));
    a.dict.emplace_back("port", BNode::num(port));
    a.dict.emplace_back("implied_port", BNode::num(0));
    a.dict.emplace_back("token", BNode::str(token));
    return bencode(query(tid, "announce_peer", std::move(a)));
}

bool parse_reply(const std::uint8_t* d, std::size_t n, Reply* out) {
    BNode m;
    if (!bdecode(d, n, &m) || m.kind != BNode::Dict) return false;
    const BNode* t = m.get("t");
    const BNode* y = m.get("y");
    if (!t || t->kind != BNode::Str || !y || y->kind != BNode::Str) return false;
    *out = Reply{};
    out->tid = t->s;
    if (y->s == "e") {
        out->is_error = true;
        return true;
    }
    if (y->s == "q") {
        out->is_query = true;
        return true;
    }
    if (y->s != "r") return false;
    const BNode* r = m.get("r");
    if (!r || r->kind != BNode::Dict) return false;
    if (const BNode* id = r->get("id"); id && id->kind == BNode::Str && id->s.size() == 20) {
        std::memcpy(out->id.data(), id->s.data(), 20);
        out->has_id = true;
    }
    if (const BNode* tok = r->get("token"); tok && tok->kind == BNode::Str && tok->s.size() <= 64) out->token = tok->s;
    if (const BNode* v = r->get("values"); v && v->kind == BNode::List) {
        for (const BNode& x : v->list) {
            if (x.kind != BNode::Str || x.s.size() != 6) continue;
            Endpoint e;
            std::memcpy(&e.addr, x.s.data(), 4);
            std::memcpy(&e.port, x.s.data() + 4, 2);
            if (out->values.size() < 200) out->values.push_back(e);
        }
    }
    if (const BNode* ns = r->get("nodes"); ns && ns->kind == BNode::Str) {
        for (std::size_t i = 0; i + 26 <= ns->s.size() && out->nodes.size() < 64; i += 26) {
            Id id;
            Endpoint e;
            std::memcpy(id.data(), ns->s.data() + i, 20);
            std::memcpy(&e.addr, ns->s.data() + i + 20, 4);
            std::memcpy(&e.port, ns->s.data() + i + 24, 2);
            out->nodes.emplace_back(id, e);
        }
    }
    return true;
}

bool closer(const Id& t, const Id& a, const Id& b) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        const std::uint8_t da = a[i] ^ t[i], db = b[i] ^ t[i];
        if (da != db) return da < db;
    }
    return false;
}

bool usable_node(const Endpoint& e, bool allow_private) {
    if (e.port == 0) return false;
    const auto* b = reinterpret_cast<const std::uint8_t*>(&e.addr);
    if (allow_private) return b[0] != 0;
    if (b[0] == 0 || b[0] == 127 || b[0] == 10 || b[0] >= 224) return false;
    if (b[0] == 169 && b[1] == 254) return false;
    if (b[0] == 172 && (b[1] & 0xf0) == 16) return false;
    if (b[0] == 192 && b[1] == 168) return false;
    if (b[0] == 100 && (b[1] & 0xc0) == 64) return false;  // CGNAT shared space
    return true;
}

std::vector<std::string> default_bootstrap() {
    return {"router.bittorrent.com:6881", "dht.transmissionbt.com:6881", "router.utorrent.com:6881",
            "dht.libtorrent.org:25401"};
}

// ---- Lookup ----

Lookup::Lookup(const Id& target, const Id& self, Options opt, Clock::time_point now)
    : target_(target), self_(self), opt_(opt), start_(now) {}

void Lookup::seed(const Endpoint& e) {
    if (!usable_node(e, opt_.allow_private)) return;
    for (const Node& n : nodes_)
        if (n.ep == e) return;
    Node n;
    n.ep = e;
    nodes_.push_back(n);
}

void Lookup::add_node(const Id& id, const Endpoint& ep) {
    if (!usable_node(ep, opt_.allow_private) || nodes_.size() >= 512 || id == self_) return;
    for (Node& n : nodes_)
        if (n.ep == ep) {
            if (!n.has_id) {
                n.id = id;
                n.has_id = true;
            }
            return;
        }
    Node n;
    n.ep = ep;
    n.id = id;
    n.has_id = true;
    nodes_.push_back(n);
}

Lookup::Node* Lookup::by_tid(const std::string& tid) {
    for (Node& n : nodes_)
        if (n.tid == tid) return &n;
    return nullptr;
}

std::string Lookup::next_tid() {
    ++tid_;
    return std::string{static_cast<char>(tid_ >> 8), static_cast<char>(tid_ & 0xff)};
}

std::vector<Lookup::Node*> Lookup::ranked() {
    std::vector<Node*> with, without;
    for (Node& n : nodes_) (n.has_id ? with : without).push_back(&n);
    std::sort(with.begin(), with.end(), [&](Node* a, Node* b) { return closer(target_, a->id, b->id); });
    with.insert(with.end(), without.begin(), without.end());
    return with;
}

std::vector<Endpoint> Lookup::closest() const {
    std::vector<const Node*> v;
    for (const Node& n : nodes_)
        if (n.st == NState::Answered && n.has_id) v.push_back(&n);
    std::sort(v.begin(), v.end(), [&](const Node* a, const Node* b) { return closer(target_, a->id, b->id); });
    std::vector<Endpoint> out;
    for (std::size_t i = 0; i < v.size() && i < opt_.k; ++i) out.push_back(v[i]->ep);
    return out;
}

void Lookup::on_datagram(const Endpoint& from, const std::uint8_t* d, std::size_t n, Clock::time_point now) {
    (void)now;
    Reply r;
    if (phase_ == Phase::Done || !parse_reply(d, n, &r) || r.is_query) return;
    Node* node = by_tid(r.tid);
    if (!node || !(node->ep == from)) return;
    node->tid.clear();
    if (phase_ == Phase::Announce) {
        if (!r.is_error) ++announced_;
        return;
    }
    if (node->st != NState::InFlight) return;
    if (r.is_error) {
        node->st = NState::Failed;
        return;
    }
    node->st = NState::Answered;
    ++responses_;
    if (r.has_id) {
        node->id = r.id;
        node->has_id = true;
    }
    node->token = r.token;
    for (const Endpoint& e : r.values) {
        if (!usable_node(e, opt_.allow_private)) continue;
        if (std::find(peers_.begin(), peers_.end(), e) == peers_.end() && peers_.size() < 64) peers_.push_back(e);
    }
    for (const auto& [id, ep] : r.nodes) add_node(id, ep);
}

void Lookup::poll(Clock::time_point now, std::vector<Datagram>& out) {
    using std::chrono::milliseconds;
    if (phase_ == Phase::Done) return;
    if (phase_ == Phase::Announce) {
        if (now >= announce_until_ || announced_ >= announce_sent_) phase_ = Phase::Done;
        return;
    }
    int in_flight = 0;
    for (Node& n : nodes_) {
        if (n.st != NState::InFlight) continue;
        if (now - n.sent > milliseconds(opt_.query_timeout_ms)) {
            n.st = n.tries < 2 ? NState::Fresh : NState::Failed;  // one resend: a lost datagram
            n.tid.clear();
        } else {
            ++in_flight;
        }
    }
    const std::vector<Node*> r = ranked();
    bool finished = now - start_ > milliseconds(opt_.total_ms);
    if (!finished) {
        std::size_t seen = 0;
        bool all_answered = true, any_left = false;
        for (Node* n : r) {
            if (n->st == NState::Failed) continue;
            if (seen++ >= opt_.k) break;
            if (n->st != NState::Answered) all_answered = false;
            if (n->st == NState::Fresh) {
                any_left = true;
                if (in_flight < opt_.alpha) {
                    n->tid = next_tid();
                    n->st = NState::InFlight;
                    n->sent = now;
                    ++n->tries;
                    ++in_flight;
                    ++queried_;
                    const std::string m = make_get_peers(n->tid, self_, target_);
                    out.push_back({n->ep, std::vector<std::uint8_t>(m.begin(), m.end())});
                }
            }
        }
        if ((seen > 0 && all_answered) || (!any_left && in_flight == 0)) finished = true;
    }
    if (!finished) return;
    if (!opt_.announce) {
        phase_ = Phase::Done;
        return;
    }
    phase_ = Phase::Announce;
    announce_until_ = now + milliseconds(opt_.query_timeout_ms);
    std::size_t sent = 0;
    for (Node* n : r) {
        if (sent >= opt_.k) break;
        if (n->st != NState::Answered || n->token.empty()) continue;
        n->tid = next_tid();
        const std::string m = make_announce(n->tid, self_, target_, opt_.announce_port, n->token);
        out.push_back({n->ep, std::vector<std::uint8_t>(m.begin(), m.end())});
        ++sent;
    }
    announce_sent_ = static_cast<int>(sent);
    if (!sent) phase_ = Phase::Done;
}

// ---- Rendezvous ----

Rendezvous::Rendezvous(const Id& target, std::vector<std::string> bootstrap, bool loopback, Log log)
    : target_(target), bootstrap_(std::move(bootstrap)), loopback_(loopback), log_(std::move(log)) {
    crypto::random_bytes(self_.data(), self_.size());
}

Rendezvous::~Rendezvous() { stop(); }

bool Rendezvous::open_socket() {
    if (!sock::startup()) return false;
    sock::Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == sock::kInvalid) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = loopback_ || sock::loopback_only() ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        sock::close(s);
        return false;
    }
    sock::set_nonblocking(s);
    sock_ = static_cast<std::intptr_t>(s);
    return true;
}

bool Rendezvous::start_host(OnPeer on_peer, int period_ms) {
    if (th_.joinable() || !open_socket()) return false;
    on_peer_ = std::move(on_peer);
    th_ = std::thread([this, period_ms] { run(true, 0, period_ms); });
    return true;
}

bool Rendezvous::start_guest(std::uint16_t port, std::function<bool()> want, int period_ms) {
    if (th_.joinable() || !open_socket()) return false;
    want_ = std::move(want);
    th_ = std::thread([this, port, period_ms] { run(false, port, period_ms); });
    return true;
}

void Rendezvous::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (sock_ != -1) {
        sock::close(static_cast<sock::Socket>(sock_));
        sock_ = -1;
    }
}

void Rendezvous::run(bool host, std::uint16_t port, int period_ms) {
    using std::chrono::milliseconds;
    const sock::Socket s = static_cast<sock::Socket>(sock_);
    std::vector<Endpoint> boot, prev;
    auto resolve_boot = [&] {
        boot.clear();
        for (const std::string& b : bootstrap_) {
            std::array<std::uint8_t, 4> ip{};
            std::uint16_t p = 0;
            if (!resolve_ipv4(b, 6881, &ip, &p)) continue;
            Endpoint e;
            std::memcpy(&e.addr, ip.data(), 4);
            e.port = htons(p);
            boot.push_back(e);
        }
    };
    std::unique_ptr<Lookup> lk;
    Clock::time_point next = Clock::now();
    bool reported_empty = false, first = true;
    std::vector<Endpoint> told;  // peers already logged
    while (!stop_) {
        const Clock::time_point now = Clock::now();
        if (!lk && now >= next && (host || (want_ && want_()))) {
            if (boot.empty()) resolve_boot();
            Lookup::Options o;
            o.announce = !host;
            o.announce_port = port;
            o.allow_private = loopback_;
            Id me = self_;
            lk = std::make_unique<Lookup>(target_, me, o, now);
            for (const Endpoint& e : prev) lk->seed(e);
            for (const Endpoint& e : boot) lk->seed(e);
            if (boot.empty() && prev.empty()) {
                if (!reported_empty && log_) log_("rendezvous: no DHT bootstrap node resolves (offline?)");
                reported_empty = true;
                lk.reset();
                next = now + milliseconds(period_ms);
                continue;
            }
        }
        if (lk) {
            std::vector<Datagram> out;
            lk->poll(now, out);
            for (const Datagram& d : out) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = d.to.addr;
                to.sin_port = d.to.port;
                ::sendto(s, reinterpret_cast<const char*>(d.data.data()), static_cast<int>(d.data.size()), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof to);
            }
            if (lk->done()) {
                ++lookups_;
                const std::vector<Endpoint> c = lk->closest();
                if (!c.empty()) prev = c;
                if (host) {
                    for (const Endpoint& e : lk->peers()) {
                        if (log_ && std::find(told.begin(), told.end(), e) == told.end()) {
                            told.push_back(e);
                            const auto* b = reinterpret_cast<const std::uint8_t*>(&e.addr);
                            log_("rendezvous: a guest's endpoint " + std::to_string(b[0]) + "." + std::to_string(b[1]) +
                                 "." + std::to_string(b[2]) + "." + std::to_string(b[3]) + ":" +
                                 std::to_string(ntohs(e.port)) + " found in the DHT");
                        }
                        if (on_peer_) on_peer_(e);
                    }
                } else {
                    announces_ += lk->announced();
                }
                if (log_ && (first || (!host) || lk->responses() == 0))
                    log_(std::string("rendezvous: ") + (host ? "lookup" : "announce") + " done: " +
                         std::to_string(lk->responses()) + " DHT nodes answered of " + std::to_string(lk->queried()) +
                         (host ? ", " + std::to_string(lk->peers().size()) + " guest endpoint(s)"
                               : ", stored at " + std::to_string(lk->announced())));
                first = false;
                lk.reset();
                next = Clock::now() + milliseconds(period_ms);
            }
        }
        sock::PollFd pfd{};
        pfd.fd = s;
        pfd.events = POLLIN;
        if (sock::poll(&pfd, 1, lk ? 20 : 250) > 0) {
            for (int i = 0; i < 64; ++i) {
                std::uint8_t buf[2048];
                sockaddr_in from{};
                sock::socklen fl = sizeof from;
                const long r = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                          reinterpret_cast<sockaddr*>(&from), &fl);
                if (r <= 0) break;
                if (lk) lk->on_datagram({from.sin_addr.s_addr, from.sin_port}, buf, static_cast<std::size_t>(r), Clock::now());
            }
        }
    }
}

}  // namespace party::rdv
