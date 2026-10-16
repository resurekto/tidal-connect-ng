#pragma once
// QueueItem: one entry of a queue window. Parsed from the TIDAL queue server's
// snake_case JSON (cloud) or the app's camelCase MediaInfo (local).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tcng::queue {

class QueueItem {
public:
    struct Image {
        std::string url;
        int width = 0;
        int height = 0;
    };
    struct Policy {
        bool canNext = true;
        bool canPrevious = true;
    };
    enum class MediaType : std::uint32_t { Track = 0, Video = 1 };

    QueueItem() = default;
    QueueItem(const nlohmann::json& j, bool local);

    const std::string& getItemId() const { return itemId_; }
    const std::string& getMediaId() const { return mediaId_; }
    const std::string& getSrcUrl() const { return srcUrl_; }
    const std::string& getTitle() const { return title_; }
    const std::vector<std::string>& getArtists() const { return artists_; }
    const std::string& getAlbumTitle() const { return albumTitle_; }
    std::uint64_t getDuration() const { return duration_; }
    const std::map<std::string, Image>& getImages() const { return images_; }
    const Policy& getPolicy() const { return policy_; }
    const std::map<std::string, std::string>& getProperties() const { return properties_; }
    MediaType getMediaType() const { return mediaType_; }
    bool isActive() const { return active_; }

    std::string getInfo() const;

    // The app-format mediaInfo sent to playback.
    nlohmann::json makeMediaInfo() const;

private:
    void parseCloud(const nlohmann::json& j);
    void parseLocal(const nlohmann::json& j);

    std::string itemId_;
    std::string mediaId_;
    std::string srcUrl_;
    std::string title_;
    std::vector<std::string> artists_;
    std::string albumTitle_;
    std::uint64_t duration_ = 0;
    std::map<std::string, Image> images_;
    Policy policy_;
    std::map<std::string, std::string> properties_;
    MediaType mediaType_ = MediaType::Track;
    bool active_ = false;
};

}  // namespace tcng::queue
