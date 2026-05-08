#include "FavoriteMusicManager.h"
#include "AudioManager.h"

#include "Log.h"
#include "Settings.h"
#include "Sound.h"
#include <SDL.h>
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#include "utils/Randomizer.h"
#include "SystemConf.h"
#include "id3v2lib/include/id3v2lib.h"
#include "ThemeData.h"
#include "Paths.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>

#ifdef WIN32
#include <time.h>
#else
#include <unistd.h>
#include <errno.h>
#include <iconv.h>
#endif

// batocera
// Size of last played music history as a percentage of file total
#define LAST_PLAYED_SIZE 0.4

AudioManager* AudioManager::sInstance = NULL;
std::vector<std::shared_ptr<Sound>> AudioManager::sSoundVector;

AudioManager::AudioManager() : mInitialized(false), mCurrentMusic(nullptr), mMusicVolume(MIX_MAX_VOLUME), mVideoPlaying(false)
{
	init();
}

AudioManager::~AudioManager()
{
	deinit();
}

AudioManager* AudioManager::getInstance()
{
	//check if an AudioManager instance is already created, if not create one
	if (sInstance == nullptr)
		sInstance = new AudioManager();

	return sInstance;
}

bool AudioManager::isInitialized()
{
	if (sInstance == nullptr)
		return false;

	return sInstance->mInitialized;
}

void AudioManager::init()
{
	if (mInitialized)
		return;
	
	mSongNameChanged = false;
	mPlayingSystemThemeSong = "none";
	std::deque<std::string> mLastPlayed;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		LOG(LogError) << "Error initializing SDL audio!\n" << SDL_GetError();
		return;
	}

	// Open the audio device and pause
	if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0)
	{
		mMusicVolume = 0;
		LOG(LogError) << "MUSIC Error - Unable to open SDLMixer audio: " << SDL_GetError() << std::endl;
	}
	else
	{
		LOG(LogInfo) << "SDL AUDIO Initialized";
		mInitialized = true;

		// Reload known sounds
		for (unsigned int i = 0; i < sSoundVector.size(); i++)
			sSoundVector[i]->init();

		mMusicVolume = getMaxMusicVolume();
		Mix_VolumeMusic(mMusicVolume);
	}
}


void AudioManager::deinit()
{
	if (!mInitialized)
		return;

	LOG(LogDebug) << "AudioManager::deinit";

	mInitialized = false;

	//stop all playback
	stop();
	stopMusic();

	// Free known sounds from memory
	for (unsigned int i = 0; i < sSoundVector.size(); i++)
		sSoundVector[i]->deinit();

	Mix_HookMusicFinished(nullptr);
	Mix_HaltMusic();

	//completely tear down SDL audio. else SDL hogs audio resources and emulators might fail to start...
	Mix_CloseAudio();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);

	LOG(LogInfo) << "SDL AUDIO Deinitialized";
}

void AudioManager::registerSound(std::shared_ptr<Sound> & sound)
{
	getInstance();
	sSoundVector.push_back(sound);
}

void AudioManager::unregisterSound(std::shared_ptr<Sound> & sound)
{
	getInstance();
	for (unsigned int i = 0; i < sSoundVector.size(); i++)
	{
		if (sSoundVector.at(i) == sound)
		{
			sSoundVector[i]->stop();
			sSoundVector.erase(sSoundVector.cbegin() + i);
			return;
		}
	}
	LOG(LogWarning) << "AudioManager Error - tried to unregister a sound that wasn't registered!";
}

void AudioManager::play()
{
	getInstance();
}

void AudioManager::stop()
{
	// Stop playing all Sounds
	for (unsigned int i = 0; i < sSoundVector.size(); i++)
		if (sSoundVector.at(i)->isPlaying())
			sSoundVector[i]->stop();
}

void AudioManager::getMusicIn(const std::string &path, std::vector<std::string>& all_matching_files)
{
	if (!Utils::FileSystem::isDirectory(path))
		return;

	bool anySystem = !Settings::getInstance()->getBool("audio.persystem");

	auto dirContent = Utils::FileSystem::getDirContent(path);
	for (auto it = dirContent.cbegin(); it != dirContent.cend(); ++it)
	{
		if (Utils::FileSystem::isDirectory(*it))
		{
			if (*it == "." || *it == "..")
				continue;

			if (anySystem || mSystemName == Utils::FileSystem::getFileName(*it))
				getMusicIn(*it, all_matching_files);
		}
		else if (Utils::FileSystem::isAudio(*it))
			all_matching_files.push_back(*it);
	}
}

// batocera
// Add the current song to the last played history, truncating as needed
void AudioManager::addLastPlayed(const std::string& newSong, int totalMusic)
{
	int historySize = std::floor(totalMusic * LAST_PLAYED_SIZE);
	if (historySize < 1)
	{
		// Number of songs is too small to bother with
		return;
	}
	
	while (mLastPlayed.size() > historySize) {
		mLastPlayed.pop_back();
	}
	mLastPlayed.push_front(newSong);
	
	LOG(LogDebug) << "Adding " << newSong << " to last played, " << mLastPlayed.size() << " in history";
}

// batocera
// Check if current song exists in last played history
bool AudioManager::songWasPlayedRecently(const std::string& song)
{
	for (std::string i : mLastPlayed)
	{
		if (song == i)
		{
			return true;
		}
	}
	return false;
}

void AudioManager::playRandomMusic(bool continueIfPlaying)
{
    if (!Settings::BackgroundMusic())
        return;

    if (Settings::getInstance()->getBool("audio.useFavoriteMusic"))
    {
        std::string favoritesFile = FavoriteMusicManager::getFavoriteMusicFilePath();
        auto favorites = FavoriteMusicManager::loadFavoriteSongs(favoritesFile);

        if (favorites.empty())
        {
            LOG(LogInfo) << "No favorite music found in " << favoritesFile;
            Settings::getInstance()->setBool("audio.useFavoriteMusic", false);
            Settings::getInstance()->saveFile();
        }
        else
        {
            // Normal favorite playback logic
            int randomIndex = Randomizer::random(favorites.size());
            std::string chosenSongPath = favorites[randomIndex].first;

            if (mCurrentMusic != nullptr && continueIfPlaying)
                return;

            LOG(LogInfo) << "Playing favorite music: " << favorites[randomIndex].second
                        << " (" << chosenSongPath << ")";
            playMusic(chosenSongPath);
            playSong(chosenSongPath);
            addLastPlayed(chosenSongPath, favorites.size());
            mPlayingSystemThemeSong = "";
            return;
        }
    }

    std::vector<std::string> musics;

    if (!mCurrentThemeMusicDirectory.empty())
        getMusicIn(mCurrentThemeMusicDirectory, musics);

    if (musics.empty())
        getMusicIn(Paths::getUserMusicPath(), musics);

    if (musics.empty())
        getMusicIn(Paths::getMusicPath(), musics);

    if (musics.empty())
        getMusicIn(Paths::getUserEmulationStationPath() + "/music", musics);

    if (musics.empty())
        return;

    int randomIndex = Randomizer::random(musics.size());
    while (songWasPlayedRecently(musics.at(randomIndex)))
    {
        LOG(LogDebug) << "Music "" << musics.at(randomIndex) << "" was played recently, trying again";
        randomIndex = Randomizer::random(musics.size());
    }

    if (mCurrentMusic != nullptr && continueIfPlaying)
        return;

    playMusic(musics.at(randomIndex));
    playSong(musics.at(randomIndex));
    addLastPlayed(musics.at(randomIndex), musics.size());
    mPlayingSystemThemeSong = "";
}

void AudioManager::playMusic(std::string path)
{
	if (!mInitialized)
		return;

	// free the previous music
	stopMusic(false);

	if (!Settings::BackgroundMusic())
		return;

	// load a new music
	mCurrentMusic = Mix_LoadMUS(path.c_str());
	if (mCurrentMusic == NULL)
	{
		LOG(LogError) << Mix_GetError() << " for " << path;
		return;
	}

	if (Mix_FadeInMusic(mCurrentMusic, 1, 1000) == -1)
	{
		stopMusic();
		return;
	}

	mCurrentMusicPath = path;
	Mix_HookMusicFinished(AudioManager::musicEnd_callback);
}

void AudioManager::musicEnd_callback()
{
	if (!AudioManager::getInstance()->mPlayingSystemThemeSong.empty())
	{
		AudioManager::getInstance()->playMusic(AudioManager::getInstance()->mPlayingSystemThemeSong);
		return;
	}

	AudioManager::getInstance()->playRandomMusic(false);
}

void AudioManager::stopMusic(bool fadeOut)
{
	if (mCurrentMusic == NULL)
		return;

	Mix_HookMusicFinished(nullptr);

	if (fadeOut)
	{
		// Fade-out is nicer !
		while (!Mix_FadeOutMusic(500) && Mix_PlayingMusic())
			SDL_Delay(100);
	}

	Mix_HaltMusic();
	Mix_FreeMusic(mCurrentMusic);
	mCurrentMusicPath = "";
	mCurrentMusic = NULL;
}

std::string AudioManager::getCurrentSongPath() const
{
    return mCurrentMusicPath;  
}

// Fast string hash in order to use strings in switch/case
// How does this work? Look for Dan Bernstein hash on the internet
constexpr unsigned int sthash(const char *s, int off = 0)
{
	return !s[off] ? 5381 : (sthash(s, off+1)*33) ^ s[off];
}

namespace {

void appendUtf8Codepoint(std::string& s, uint32_t cp)
{
	if (cp <= 0x7F)
		s.push_back((char)cp);
	else if (cp <= 0x7FF)
	{
		s.push_back((char)(0xC0 | ((cp >> 6) & 0x1F)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	}
	else if (cp <= 0xFFFF)
	{
		s.push_back((char)(0xE0 | ((cp >> 12) & 0x0F)));
		s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	}
	else
	{
		s.push_back((char)(0xF0 | ((cp >> 18) & 0x07)));
		s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		s.push_back((char)(0x80 | (cp & 0x3F)));
	}
}

std::string latin1BytesToUtf8(const char* data, size_t len)
{
	std::string out;
	for (size_t i = 0; i < len; i++)
	{
		const unsigned char c = (unsigned char)data[i];
		if (c == 0)
			break;
		if (c < 0x80)
			out.push_back((char)c);
		else
		{
			out.push_back((char)(0xC0 | (c >> 6)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

bool isValidUtf8(const std::string& s)
{
	const unsigned char* b = reinterpret_cast<const unsigned char*>(s.data());
	const size_t len = s.size();
	for (size_t i = 0; i < len;)
	{
		const unsigned char c = b[i];
		if (c <= 0x7FU)
		{
			++i;
			continue;
		}
		if (c < 0xC0U)
			return false;
		if (c <= 0xDFU)
		{
			if (i + 1U >= len || (b[i + 1] & 0xC0U) != 0x80U)
				return false;
			i += 2;
			continue;
		}
		if (c <= 0xEFU)
		{
			if (i + 2U >= len || (b[i + 1] & 0xC0U) != 0x80U || (b[i + 2] & 0xC0U) != 0x80U)
				return false;
			i += 3;
			continue;
		}
		if (c <= 0xF4U)
		{
			if (i + 3U >= len || (b[i + 1] & 0xC0U) != 0x80U || (b[i + 2] & 0xC0U) != 0x80U
			    || (b[i + 3] & 0xC0U) != 0x80U)
				return false;
			i += 4;
			continue;
		}
		return false;
	}
	return true;
}

#if !defined(WIN32)
std::string iconvBytesToUtf8(const char* fromCode, const std::string& in)
{
	if (in.empty())
		return "";
	iconv_t cd = iconv_open("UTF-8", fromCode);
	if (cd == (iconv_t)-1)
		return "";
	std::vector<char> inbuf(in.begin(), in.end());
	char* inptr = inbuf.data();
	size_t inleft = inbuf.size();
	std::vector<char> outbuf(in.size() * 8 + 64);
	char* outbase = outbuf.data();
	char* outptr = outbase;
	size_t outleft = outbuf.size();

	errno = 0;
	if (iconv(cd, &inptr, &inleft, &outptr, &outleft) == (size_t)-1)
	{
		iconv_close(cd);
		return "";
	}
	iconv_close(cd);
	if (inleft != 0)
		return "";
	return std::string(outbase, outptr);
}
#else
std::string iconvBytesToUtf8(const char* /*fromCode*/, const std::string& /*in*/)
{
	return "";
}
#endif

bool localeUsesChineseId3EncodingFallback()
{
	const std::string lang = SystemConf::getInstance()->get("system.language");
	if (lang.size() < 2)
		return false;
	const unsigned char a = (unsigned char)lang[0];
	const unsigned char b = (unsigned char)lang[1];
	return (a == 'z' || a == 'Z') && (b == 'h' || b == 'H');
}

std::string decodeLegacyId3Text(const std::string& raw)
{
	if (raw.empty())
		return "";
	if (isValidUtf8(raw))
		return raw;
#if !defined(WIN32)
	if (localeUsesChineseId3EncodingFallback())
	{
		std::string fromGb = iconvBytesToUtf8("GB18030//IGNORE", raw);
		if (fromGb.empty())
			fromGb = iconvBytesToUtf8("GB18030", raw);
		if (!fromGb.empty() && isValidUtf8(fromGb))
			return fromGb;
	}
#endif
	return latin1BytesToUtf8(raw.data(), raw.size());
}

std::string id3v1FieldToUtf8(const char* field, size_t maxLen)
{
	size_t len = maxLen;
	while (len > 0 && (unsigned char)field[len - 1] <= 0x20U)
		--len;
	if (len == 0)
		return "";
	return decodeLegacyId3Text(std::string(field, len));
}

std::string utf16BytesToUtf8(const char* data, size_t byteLen, bool forceBigEndian)
{
	std::string out;
	size_t i = 0;
	bool bigEndian = forceBigEndian;
	if (byteLen >= 2)
	{
		const unsigned char b0 = (unsigned char)data[0], b1 = (unsigned char)data[1];
		if (b0 == 0xFF && b1 == 0xFE)
		{
			i = 2;
			bigEndian = false;
		}
		else if (b0 == 0xFE && b1 == 0xFF)
		{
			i = 2;
			bigEndian = true;
		}
	}
	while (i + 1 < byteLen)
	{
		uint16_t u = bigEndian ? (uint16_t)(((unsigned char)data[i] << 8) | (unsigned char)data[i + 1])
		                      : (uint16_t)((unsigned char)data[i] | ((unsigned char)data[i + 1] << 8));
		i += 2;
		if (u == 0)
			break;
		if (u >= 0xD800 && u <= 0xDBFF && i + 1 < byteLen)
		{
			const uint16_t low = bigEndian ? (uint16_t)(((unsigned char)data[i] << 8) | (unsigned char)data[i + 1])
			                               : (uint16_t)((unsigned char)data[i] | ((unsigned char)data[i + 1] << 8));
			if (low >= 0xDC00 && low <= 0xDFFF)
			{
				i += 2;
				const uint32_t cp = 0x10000u + ((((uint32_t)(u - 0xD800)) << 10) | (low - 0xDC00));
				appendUtf8Codepoint(out, cp);
				continue;
			}
		}
		appendUtf8Codepoint(out, u);
	}
	return out;
}

// ID3v2 text frame per encoding byte: 0 Latin-1, 1 UTF-16 BOM, 2 UTF-16BE, 3 UTF-8
std::string id3TextContentToUtf8(ID3v2_frame_text_content* tc)
{
	if (tc == nullptr || tc->data == nullptr || tc->size <= 0)
		return "";
	const unsigned char enc = (unsigned char)tc->encoding;
	const char* d = tc->data;
	const size_t n = (size_t)tc->size;

	switch (enc)
	{
		case 0:
		{
			size_t end = n;
			for (size_t j = 0; j < n; j++)
				if (d[j] == '\0')
				{
					end = j;
					break;
				}
			return decodeLegacyId3Text(std::string(d, end));
		}
		case 3:
		{
			size_t end = n;
			for (size_t j = 0; j < n; j++)
				if (d[j] == '\0')
				{
					end = j;
					break;
				}
			return std::string(d, end);
		}
		case 1:
			return utf16BytesToUtf8(d, n, false);
		case 2:
			return utf16BytesToUtf8(d, n, true);
		default:
			return std::string(d, n);
	}
}

} // namespace

void AudioManager::setSongName(const std::string& song)
{
	if (song == mCurrentSong)
		return;

	mCurrentSong = song;
	mSongNameChanged = true;
}

void AudioManager::playSong(const std::string& song)
{
	if (song == mCurrentSong)
		return;

	if (song.empty())
	{
		mSongNameChanged = true;
		mCurrentSong = "";
		return;
	}

	std::string ext = Utils::String::toLower(Utils::FileSystem::getExtension(song));
	// chiptunes mod song titles parsing
	if (ext == ".mod" || ext == ".s3m" || ext == ".stm" || ext == ".669" || ext == ".mtm" || ext == ".far" || ext == ".xm" || ext == ".it" )
	{
		int title_offset;
		int title_break;
		struct {
			char title[108] = "";
		} info;
		switch (sthash(ext.c_str())) {
			case sthash(".mod"):
			case sthash(".stm"):
				title_offset = 0;
				title_break = 20;
				break;
			case sthash(".s3m"):
				title_offset = 0;
				title_break = 28;
				break;
			case sthash(".669"):
				title_offset = 0;
				title_break = 108;
				break;
			case sthash(".mtm"):
			case sthash(".it"):
				title_offset = 4;
				title_break = 20;
				break;
			case sthash(".far"):
				title_offset = 4;
				title_break = 40;
				break;
			case sthash(".xm"):
				title_offset = 17;
				title_break = 20;
				break;
			default:
				LOG(LogError) << "Error AudioManager unexpected case while loading mofile " << song;
				setSongName(Utils::FileSystem::getStem(song.c_str()));				
				return;
		}

		FILE* file = fopen(song.c_str(), "r");
		if (file != NULL)
		{
			if (fseek(file, title_offset, SEEK_SET) < 0)
				LOG(LogError) << "Error AudioManager seeking " << song;
			else if (fread(&info, sizeof(info), 1, file) != 1)
				LOG(LogError) << "Error AudioManager reading " << song;
			else  
			{
				info.title[title_break] = '\0';

				std::string name = info.title;
				if (!name.empty())
				{
					setSongName(name);
					fclose(file);
					return;
				}
			}

			fclose(file);
		}
		else
			LOG(LogError) << "Error AudioManager opening modfile " << song;
	}

	// now only mp3 will be parsed for ID3: .ogg, .wav and .flac will display file name
	if (ext != ".mp3")
	{
		setSongName(Utils::FileSystem::getStem(song.c_str()));
		return;
	}

	LOG(LogDebug) << "AudioManager::setSongName";

	// First let's try with an ID3 v2 tag
	ID3v2_tag* tag = load_tag(song.c_str());
	if (tag != NULL)
	{
		ID3v2_frame* title_frame = tag_get_title(tag);
		if (title_frame != NULL)
		{
			ID3v2_frame_text_content* title_content = parse_text_frame_content(title_frame);
			if (title_content != NULL && title_content->size > 0)
			{
				std::string song_name = id3TextContentToUtf8(title_content);
				ID3v2_frame* artist_frame = tag_get_artist(tag);
				if (artist_frame != NULL)
				{
					ID3v2_frame_text_content* artist_content = parse_text_frame_content(artist_frame);
					if (artist_content != NULL && artist_content->size > 0)
					{
						std::string artist = id3TextContentToUtf8(artist_content);
						song_name += " - " + artist;
						free(artist_content->data);
						free(artist_content);
					}
				}
				setSongName(song_name);
				free(title_content->data);
				free(title_content);
				free_tag(tag);
				return;
			}
		}
		free_tag(tag);
	}

	// Then, if no v2, let's try with an ID3 v1 tag	
	struct {
		char tag[3];	// i.e. "TAG"
		char title[30];
		char artist[30];
		char album[30];
		char year[4];
		char comment[30];
		unsigned char genre;
	} info;

	FILE* file = fopen(song.c_str(), "r");
	if (file != NULL)
	{
		if (fseek(file, -128, SEEK_END) < 0)
			LOG(LogError) << "Error AudioManager seeking " << song;
		else if (fread(&info, sizeof(info), 1, file) != 1)
			LOG(LogError) << "Error AudioManager reading " << song;
		else if (strncmp(info.tag, "TAG", 3) == 0)
		{
			std::string titleU = id3v1FieldToUtf8(info.title, 30);
			std::string artistU = id3v1FieldToUtf8(info.artist, 30);
			std::string songLine;
			if (!titleU.empty() && !artistU.empty())
				songLine = titleU + " - " + artistU;
			else if (!titleU.empty())
				songLine = titleU;
			else
				songLine = artistU;
			if (songLine.empty())
				songLine = Utils::FileSystem::getStem(song.c_str());
			setSongName(songLine);
			fclose(file);
			return;
		}

		fclose(file);
	}
	else
		LOG(LogError) << "Error AudioManager opening mp3 file " << song;

	setSongName(Utils::FileSystem::getStem(song.c_str()));
}

void AudioManager::changePlaylist(const std::shared_ptr<ThemeData>& theme, bool force)
{
	if (theme == nullptr)
		return;

	if (!force && mSystemName == theme->getSystemThemeFolder())
		return;

	mSystemName = theme->getSystemThemeFolder();
	mCurrentThemeMusicDirectory = "";

	if (!Settings::BackgroundMusic())
		return;

	const ThemeData::ThemeElement* elem = theme->getElement("system", "directory", "sound");

	if (Settings::getInstance()->getBool("audio.thememusics"))
	{
		if (elem && elem->has("path") && !Settings::getInstance()->getBool("audio.persystem"))
			mCurrentThemeMusicDirectory = elem->get<std::string>("path");

		std::string bgSound;

		elem = theme->getElement("system", "bgsound", "sound");
		if (elem && elem->has("path") && Utils::FileSystem::exists(elem->get<std::string>("path")))
		{
			bgSound = Utils::FileSystem::getCanonicalPath(elem->get<std::string>("path"));
			if (bgSound == mCurrentMusicPath)
				return;
		}

		// Found a music for the system
		if (!bgSound.empty())
		{
			mPlayingSystemThemeSong = bgSound;
			playMusic(bgSound);			
			return;
		}
	}
	
	if (force || !mPlayingSystemThemeSong.empty() || Settings::getInstance()->getBool("audio.persystem"))
		playRandomMusic(false);
}

void AudioManager::setVideoPlaying(bool state)
{
	if (sInstance == nullptr || !sInstance->mInitialized || !Settings::BackgroundMusic())
		return;
	
	if (state && (!Settings::getInstance()->getBool("VideoLowersMusic") || !Settings::getInstance()->getBool("VideoAudio")))
	{
		sInstance->mVideoPlaying = false;
		return;
	}

	sInstance->mVideoPlaying = state;
}

int AudioManager::getMaxMusicVolume()
{
	int linearVolume = Settings::getInstance()->getInt("MusicVolume");
	double logarithmicVolume = (linearVolume == 0) ? 0 : std::pow(10.0, (linearVolume - 100) / 40.0) * MIX_MAX_VOLUME;
	int ret = static_cast<int>(logarithmicVolume);
	if (ret > MIX_MAX_VOLUME)
		return MIX_MAX_VOLUME;

	if (ret < 0)
		return 0;

	return ret;
}

void AudioManager::update(int deltaTime)
{
	if (sInstance == nullptr || !sInstance->mInitialized || !Settings::BackgroundMusic())
		return;

	float deltaVol = deltaTime / 8.0f;

//	#define MINVOL 5

	int maxVol = getMaxMusicVolume();
	int minVol = maxVol / 20;
	if (maxVol > 0 && minVol == 0)
		minVol = 1;

	if (sInstance->mVideoPlaying && sInstance->mMusicVolume != minVol)
	{		
		if (sInstance->mMusicVolume > minVol)
		{
			sInstance->mMusicVolume -= deltaVol;
			if (sInstance->mMusicVolume < minVol)
				sInstance->mMusicVolume = minVol;
		}

		Mix_VolumeMusic((int)sInstance->mMusicVolume);
	}
	else if (!sInstance->mVideoPlaying && sInstance->mMusicVolume != maxVol)
	{
		if (sInstance->mMusicVolume < maxVol)
		{
			sInstance->mMusicVolume += deltaVol;
			if (sInstance->mMusicVolume > maxVol)
				sInstance->mMusicVolume = maxVol;
		}
		else
			sInstance->mMusicVolume = maxVol;

		Mix_VolumeMusic((int)sInstance->mMusicVolume);
	}
}
