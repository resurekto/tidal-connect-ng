#include "queue/QueueItem.hpp"

#include "util/JsonUtil.hpp"

namespace tcng::queue {

using Json = nlohmann::json;

namespace {
const char* const kImageSizes[] = {"low", "medium", "high"};
}

QueueItem::QueueItem(const Json& j, bool local) {
    if (local)
        parseLocal(j);
    else
        parseCloud(j);
}

void QueueItem::parseCloud(const Json& j) {
    itemId_ = jv::string(j, "id");
    mediaId_ = jv::string(j, "media_id");
    if (!jv::at(j, "type").is_null())
        mediaType_ = jv::string(j, "type") == "video" ? MediaType::Video : MediaType::Track;
    const Json& info = jv::at(j, "display_info");
    title_ = jv::string(info, "title");
    for (const auto& a : jv::at(info, "artists"))
        if (a.is_string())
            artists_.push_back(a.get<std::string>());
    albumTitle_ = jv::string(info, "album_title");
    duration_ = static_cast<std::uint64_t>(std::max(0, jv::integer(info, "duration_ms")));
    const Json& images = jv::at(info, "images");
    for (const char* size : kImageSizes) {
        const Json& img = jv::at(images, size);
        Image image{jv::string(img, "url"), jv::integer(img, "width"),
                    jv::integer(img, "height")};
        if (!image.url.empty())
            images_[size] = image;
    }
    const Json& props = jv::at(j, "properties");
    if (props.is_object()) {
        for (auto it = props.begin(); it != props.end(); ++it) {
            const std::string value = jv::string(it.value());
            properties_[it.key()] = value;
            if (it.key() == "active")
                active_ = value == "true";
        }
    }
    srcUrl_ = jv::string(j, "src_url");
    // Cloud items carry no policy. Default: allowed.
}

void QueueItem::parseLocal(const Json& j) {
    itemId_ = jv::string(j, "itemId");
    mediaId_ = jv::string(j, "mediaId");
    srcUrl_ = jv::string(j, "srcUrl");
    const Json& meta = jv::at(j, "metadata");
    title_ = jv::string(meta, "title");
    for (const auto& a : jv::at(meta, "artists"))
        if (a.is_string())
            artists_.push_back(a.get<std::string>());
    albumTitle_ = jv::string(meta, "albumTitle");
    duration_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(jv::integer(meta, "duration")));
    const Json& images = jv::at(meta, "images");
    for (const char* size : kImageSizes) {
        const Json& img = jv::at(images, size);
        Image image{jv::string(img, "url"), jv::integer(img, "width"), jv::integer(img, "height")};
        if (!image.url.empty())
            images_[size] = image;
    }
    const Json& policy = jv::at(j, "policy");
    if (!jv::at(policy, "canNext").is_null())
        policy_.canNext = jv::boolean(policy, "canNext");
    if (!jv::at(policy, "canPrevious").is_null())
        policy_.canPrevious = jv::boolean(policy, "canPrevious");
    mediaType_ = static_cast<MediaType>(jv::integer(j, "mediaType"));
}

std::string QueueItem::getInfo() const {
    return "item_id=" + itemId_ + "/media_id=" + mediaId_ + "/active=" + (active_ ? "true/" : "false/") +
           "title=\"" + title_ + "\"";
}

Json QueueItem::makeMediaInfo() const {
    Json info = Json::object();
    if (!itemId_.empty())
        info["itemId"] = itemId_;
    if (!mediaId_.empty())
        info["mediaId"] = mediaId_;
    if (!srcUrl_.empty())
        info["srcUrl"] = srcUrl_;
    info["mediaType"] = static_cast<int>(mediaType_);

    Json meta = Json::object();
    meta["title"] = title_;
    if (!artists_.empty())
        meta["artists"] = artists_;
    if (!albumTitle_.empty())
        meta["albumTitle"] = albumTitle_;
    if (duration_ != 0)
        meta["duration"] = duration_;
    Json images = Json::object();
    for (const auto& [size, image] : images_) {
        Json img{{"url", image.url}};
        if (image.width != 0)
            img["width"] = image.width;
        if (image.height != 0)
            img["height"] = image.height;
        images[size] = img;
    }
    if (!images.empty())
        meta["images"] = images;
    info["metadata"] = meta;
    return info;
}

}  // namespace tcng::queue
