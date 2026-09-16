#include "hfs/core.hpp"

#include "common/log.hpp"

namespace hfs {

struct Client::Impl {
    Engine *engine = nullptr;
};

Client::Client(Engine &engine) : impl_(std::make_unique<Impl>()) {
    impl_->engine = &engine;
}

Client::~Client() = default;

Result<std::shared_ptr<Client>> Client::create(Engine &engine) {
    if (!engine.mounted()) {
        return makeError<std::shared_ptr<Client>>(Status::InvalidState,
                                                  "engine is not mounted");
    }
    return std::shared_ptr<Client>(new Client(engine));
}

Task<Result<void>> Client::close() {
    log::info("client closed");
    co_return Result<void>(Status::Ok);
}

} // namespace hfs
