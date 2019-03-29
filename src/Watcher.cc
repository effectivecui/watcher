#include "Watcher.hh"
#include <unordered_set>

using namespace Napi;

struct WatcherHash {
  std::size_t operator() (std::shared_ptr<Watcher> const &k) const {
    return std::hash<std::string>()(k->mDir);
  }
};

struct WatcherCompare {
  size_t operator() (std::shared_ptr<Watcher> const &a, std::shared_ptr<Watcher> const &b) const {
    return *a == *b;
  }
};

static std::unordered_set<std::shared_ptr<Watcher>, WatcherHash, WatcherCompare> sharedWatchers;

std::shared_ptr<Watcher> Watcher::getShared(std::string dir, std::unordered_set<std::string> ignore) {
  std::shared_ptr<Watcher> watcher = std::make_shared<Watcher>(dir, ignore);
  auto found = sharedWatchers.find(watcher);
  if (found != sharedWatchers.end()) {
    return *found;
  }

  sharedWatchers.insert(watcher);
  return watcher;
}

void removeShared(Watcher *watcher) {
  for (auto it = sharedWatchers.begin(); it != sharedWatchers.end(); it++) {
    if (it->get() == watcher) {
      sharedWatchers.erase(it);
      break;
    }
  }
}

Watcher::Watcher(std::string dir, std::unordered_set<std::string> ignore) 
  : mDir(dir),
    mIgnore(ignore),
    mWatched(false),
    mTree(NULL),
    mAsync(NULL),
    mCallingCallbacks(false) {
      mDebounce = Debounce::getShared();
      mDebounce->add([this] () {
        triggerCallbacks();
      });
    }

void Watcher::wait() {
  std::unique_lock<std::mutex> lk(mMutex);
  mCond.wait(lk);
}

void Watcher::notify() {
  std::unique_lock<std::mutex> lk(mMutex);
  mCond.notify_all();
  
  if (mCallbacks.size() > 0 && mEvents.size() > 0) {
    mDebounce->trigger();
  }
}

void Watcher::triggerCallbacks() {
  if (mCallbacks.size() > 0 && mEvents.size() > 0) {
    if (mCallingCallbacks) {
      mCallbackSignal.wait();
      mCallbackSignal.reset();
    }

    mCallbackEvents = mEvents;
    mEvents.clear();

    // mDebounce->trigger();
    uv_async_send(mAsync);
  }
}

void Watcher::fireCallbacks(uv_async_t *handle) {
  Watcher *watcher = (Watcher *)handle->data;
  watcher->mCallingCallbacks = true;

  watcher->mCallbacksIterator = watcher->mCallbacks.begin();
  while (watcher->mCallbacksIterator != watcher->mCallbacks.end()) {
    auto it = watcher->mCallbacksIterator;
    HandleScope scope(it->Env());
    it->Call(std::initializer_list<napi_value>{watcher->mCallbackEvents.toJS(it->Env())});

    // If the iterator was changed, then the callback trigged an unwatch.
    // The iterator will have been set to the next valid callback.
    // If it is the same as before, increment it.
    if (watcher->mCallbacksIterator == it) {
      watcher->mCallbacksIterator++;
    }
  }

  watcher->mCallingCallbacks = false;
  if (watcher->mCallbacks.size() == 0) {
    watcher->unref();
  }

  watcher->mCallbackSignal.notify();
}

bool Watcher::watch(Function callback) {
  std::unique_lock<std::mutex> lk(mMutex);
  auto res = mCallbacks.insert(Persistent(callback));
  if (res.second && !mWatched) {
    mAsync = new uv_async_t;
    mAsync->data = (void *)this;
    uv_async_init(uv_default_loop(), mAsync, Watcher::fireCallbacks);
    mWatched = true;
    return true;
  }

  return false;
}

bool Watcher::unwatch(Function callback) {
  std::unique_lock<std::mutex> lk(mMutex);

  bool removed = false;
  for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
    if (it->Value() == callback) {
      mCallbacksIterator = mCallbacks.erase(it);
      removed = true;
      break;
    }
  }
  
  if (removed && mCallbacks.size() == 0) {
    unref();
    return true;
  }

  return false;
}

void Watcher::unref() {
  if (mCallbacks.size() == 0 && !mCallingCallbacks) {
    if (mWatched) {
      mWatched = false;
      uv_close((uv_handle_t *)mAsync, Watcher::onClose);
    }

    removeShared(this);
  }
}

void Watcher::onClose(uv_handle_t *handle) {
  delete (uv_async_t *)handle;
}