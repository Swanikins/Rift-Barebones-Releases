#include "gui/CemuUpdateWindow.h"

#include "Common/RiftVersion.h"
#include "Common/version.h"
#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

#include <wx/sizer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

#include <limits>
#include <optional>
#ifndef BOOST_OS_WINDOWS
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <zip.h>


wxDECLARE_EVENT(wxEVT_RESULT, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_RESULT, wxCommandEvent);

wxDECLARE_EVENT(wxEVT_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PROGRESS, wxCommandEvent);

CemuUpdateWindow::CemuUpdateWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Rift update"), wxDefaultPosition, wxDefaultSize,
		wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_gauge->SetValue(0);
	sizer->Add(m_gauge, 0, wxALL | wxEXPAND, 5);

	auto* rows = new wxFlexGridSizer(0, 2, 0, 0);
	rows->AddGrowableCol(1);

	m_text = new wxStaticText(this, wxID_ANY, _("Checking GitHub for the latest Rift iteration..."));
	rows->Add(m_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	{
		auto* right_side = new wxBoxSizer(wxHORIZONTAL);

		m_updateButton = new wxButton(this, wxID_ANY, _("Update"));
		m_updateButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnUpdateButton, this);
		right_side->Add(m_updateButton, 0, wxALL, 5);

		m_cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
		m_cancelButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnCancelButton, this);
		right_side->Add(m_cancelButton, 0, wxALL, 5);

		rows->Add(right_side, 1, wxALIGN_RIGHT, 5);
	}

	m_changelog = new wxHyperlinkCtrl(this, wxID_ANY, _("Release notes"), wxEmptyString);
	rows->Add(m_changelog, 0, wxLEFT | wxBOTTOM | wxRIGHT | wxEXPAND, 5);

	sizer->Add(rows, 0, wxALL | wxEXPAND, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &CemuUpdateWindow::OnClose, this);
	Bind(wxEVT_RESULT, &CemuUpdateWindow::OnResult, this);
	Bind(wxEVT_PROGRESS, &CemuUpdateWindow::OnGaugeUpdate, this);
	m_thread = std::thread(&CemuUpdateWindow::WorkerThread, this);

	m_updateButton->Hide();
	m_changelog->Hide();
}

CemuUpdateWindow::~CemuUpdateWindow()
{
	m_order = WorkerOrder::Exit;
	if (m_thread.joinable())
		m_thread.join();
}

size_t CemuUpdateWindow::WriteStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	((std::string*)userdata)->append(ptr, size * nmemb);
	return size * nmemb;
};

namespace
{
	struct RiftIteration
	{
		uint32_t generation;
		uint32_t revision;
	};

	constexpr std::optional<RiftIteration> ParseRiftIteration(std::string_view tag)
	{
		constexpr std::string_view prefix = "rift-iteration-";
		if (!tag.starts_with(prefix))
			return std::nullopt;
		tag.remove_prefix(prefix.size());
		if (tag.empty())
			return std::nullopt;

		RiftIteration iteration{};
		uint32_t* value = &iteration.generation;
		bool hasDigit = false;
		bool hasRevision = false;
		for (const char character : tag)
		{
			if (character == '.')
			{
				if (!hasDigit || hasRevision)
					return std::nullopt;
				hasRevision = true;
				hasDigit = false;
				value = &iteration.revision;
				continue;
			}
			if (character < '0' || character > '9')
				return std::nullopt;
			const uint32_t digit = static_cast<uint32_t>(character - '0');
			if (*value > (std::numeric_limits<uint32_t>::max() - digit) / 10)
				return std::nullopt;
			*value = (*value * 10) + digit;
			hasDigit = true;
		}
		if (!hasDigit)
			return std::nullopt;
		return iteration;
	}

	constexpr bool IsNewerIteration(const RiftIteration& candidate, const RiftIteration& current)
	{
		return candidate.generation > current.generation ||
			(candidate.generation == current.generation && candidate.revision > current.revision);
	}

	struct RiftRelease
	{
		RiftIteration iteration;
		std::string tag;
		std::string downloadUrl;
		std::string releaseUrl;
	};

	std::optional<RiftRelease> ReadRelease(const rapidjson::Value& value, bool includePrereleases)
	{
		if (!value.IsObject() ||
			(value.HasMember("draft") && value["draft"].IsBool() && value["draft"].GetBool()) ||
			(!includePrereleases && value.HasMember("prerelease") && value["prerelease"].IsBool() && value["prerelease"].GetBool()) ||
			!value.HasMember("tag_name") || !value["tag_name"].IsString() ||
			!value.HasMember("html_url") || !value["html_url"].IsString() ||
			!value.HasMember("assets") || !value["assets"].IsArray())
		{
			return std::nullopt;
		}

		const std::string tag = value["tag_name"].GetString();
		const auto iteration = ParseRiftIteration(tag);
		if (!iteration)
			return std::nullopt;

		for (const auto& asset : value["assets"].GetArray())
		{
			if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString() ||
				!asset.HasMember("browser_download_url") || !asset["browser_download_url"].IsString())
			{
				continue;
			}

			const std::string_view name = asset["name"].GetString();
			if (!name.starts_with("Rift-Barebones-Iteration-") || !name.ends_with("-Windows-x64.zip"))
				continue;

			return RiftRelease{
				*iteration,
				tag,
				asset["browser_download_url"].GetString(),
				value["html_url"].GetString()
			};
		}

		return std::nullopt;
	}

	static_assert(ParseRiftIteration("rift-iteration-006")->revision == 0);
	static_assert(ParseRiftIteration("rift-iteration-006.12")->revision == 12);
	static_assert(IsNewerIteration(*ParseRiftIteration("rift-iteration-006.1"), *ParseRiftIteration("rift-iteration-006")));
	static_assert(IsNewerIteration(*ParseRiftIteration("rift-iteration-006.2"), *ParseRiftIteration("rift-iteration-006.1")));
	static_assert(IsNewerIteration(*ParseRiftIteration("rift-iteration-007"), *ParseRiftIteration("rift-iteration-006.99")));
	static_assert(!ParseRiftIteration("006.2"));
	static_assert(ParseRiftIteration(RiftVersion::ReleaseTag).has_value());

	void ConfigureSecureCurl(CURL* curl, char* errorBuffer)
	{
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
		curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if BOOST_OS_WINDOWS
		curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
		const auto& proxy = GetConfig().proxy_server.GetValue();
		if (!proxy.empty())
			curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
	}
}

CemuUpdateWindow::UpdateQueryResult CemuUpdateWindow::QueryUpdateInfo(std::string& downloadUrlOut,
	std::string& changelogUrlOut, std::string& latestTagOut)
{
	std::string buffer;
	constexpr std::string_view url = "https://api.github.com/repos/Swanikins/Rift-Barebones-Releases/releases?per_page=20";
	auto* curl = curl_easy_init();
	if (!curl)
		return UpdateQueryResult::Error;
	char errorBuffer[CURL_ERROR_SIZE]{};
	ConfigureSecureCurl(curl, errorBuffer);

	auto* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
	headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
	headers = curl_slist_append(headers, "Cache-Control: no-cache");
	curl_easy_setopt(curl, CURLOPT_URL, url.data());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, RiftVersion::UserAgent);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

	UpdateQueryResult result = UpdateQueryResult::Error;
	const CURLcode cr = curl_easy_perform(curl);
	if (cr == CURLE_OK)
	{
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code == 200)
		{
			rapidjson::Document releases;
			releases.Parse(buffer.data(), buffer.size());
			const auto current = ParseRiftIteration(RiftVersion::ReleaseTag);
			std::optional<RiftRelease> latest;
			if (!releases.HasParseError() && releases.IsArray() && current)
			{
				result = UpdateQueryResult::Current;
				for (const auto& value : releases.GetArray())
				{
					const auto release = ReadRelease(value, GetConfig().receive_untested_updates);
					if (!release)
						continue;
					if (!latest || IsNewerIteration(release->iteration, latest->iteration))
						latest = release;
				}
			}

			if (latest && current)
			{
				latestTagOut = latest->tag;
				if (IsNewerIteration(latest->iteration, *current))
				{
					downloadUrlOut = latest->downloadUrl;
					changelogUrlOut = latest->releaseUrl;
					result = UpdateQueryResult::Available;
				}
				else if (IsNewerIteration(*current, latest->iteration))
				{
					result = UpdateQueryResult::Ahead;
				}
			}
		}
		else
		{
			cemuLog_log(LogType::Force, "Rift update check failed with HTTP status {}", http_code);
		}
	}
	else
	{
		cemuLog_log(LogType::Force, "Rift update check failed with CURL error {} ({}): {}",
			static_cast<int>(cr), curl_easy_strerror(cr), errorBuffer);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

std::future<bool> CemuUpdateWindow::IsUpdateAvailableAsync()
{
	return std::async(std::launch::async, CheckVersion);
}

bool CemuUpdateWindow::CheckVersion()
{
	std::string downloadUrl, changelogUrl, latestTag;
	return QueryUpdateInfo(downloadUrl, changelogUrl, latestTag) == UpdateQueryResult::Available;
}


int CemuUpdateWindow::ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
	curl_off_t ulnow)
{
	auto* thisptr = (CemuUpdateWindow*)clientp;
	if (dltotal > 0)
		thisptr->m_gaugeMaxValue = static_cast<int>(std::min<curl_off_t>(dltotal, std::numeric_limits<int>::max()));
	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt(static_cast<int>(std::min<curl_off_t>(dlnow, std::numeric_limits<int>::max())));
	wxQueueEvent(thisptr, event);
	return thisptr->m_order == WorkerOrder::Exit ? 1 : 0;
}

bool CemuUpdateWindow::DownloadCemuZip(const std::string& url, const fs::path& filename)
{
	std::unique_ptr<FileStream> fsUpdateFile(FileStream::createFile2(filename));
	if (!fsUpdateFile)
		return false;

	auto* curl = curl_easy_init();
	if (!curl)
		return false;
	char errorBuffer[CURL_ERROR_SIZE]{};
	ConfigureSecureCurl(curl, errorBuffer);

	auto writeData = +[](void* ptr, size_t size, size_t nmemb, void* context) -> size_t
	{
		auto* file = static_cast<FileStream*>(context);
		const size_t writeSize = size * nmemb;
		file->writeData(ptr, writeSize);
		return writeSize;
	};

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, RiftVersion::UserAgent);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fsUpdateFile.get());
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);

	m_gaugeMaxValue = 0;
	const CURLcode curlResult = curl_easy_perform(curl);
	long httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
	curl_easy_cleanup(curl);
	fsUpdateFile.reset();
	const bool result = curlResult == CURLE_OK && httpCode >= 200 && httpCode < 300;
	if (!result)
		cemuLog_log(LogType::Force, "Rift update download failed with CURL error {} ({}) and HTTP status {}: {}",
			static_cast<int>(curlResult), curl_easy_strerror(curlResult), httpCode, errorBuffer);

	if (!result && fs::exists(filename))
	{
		try
		{
			fs::remove(filename);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "Unable to remove the incomplete Rift update: {}", ex.what());
		}
	}
	return result;
}

bool CemuUpdateWindow::ExtractUpdate(const fs::path& zipname, const fs::path& targetpath, std::string& cemuFolderName)
{
	cemuFolderName.clear();
	int err;
	auto* za = zip_open(zipname.string().c_str(), ZIP_RDONLY, &err);
	if (za == nullptr)
	{
		cemuLog_log(LogType::Force, "Cannot open zip file: {}", zipname.string());
		return false;
	}

	const auto count = zip_get_num_entries(za, 0);
	m_gaugeMaxValue = count;
	for (auto i = 0; i < count; i++)
	{
		if (m_order == WorkerOrder::Exit)
		{
			zip_close(za);
			return false;
		}

		zip_stat_t sb{};
		if (zip_stat_index(za, i, 0, &sb) == 0)
		{
			const auto len = strlen(sb.name);
			if (len == 0)
				continue;

			const fs::path relativePath = fs::path(sb.name).lexically_normal();
			if (relativePath.empty() || relativePath.is_absolute())
			{
				zip_close(za);
				return false;
			}
			for (const auto& component : relativePath)
			{
				if (component == "..")
				{
					zip_close(za);
					return false;
				}
			}

			const auto root = relativePath.begin();
			if (root == relativePath.end())
				continue;
			const std::string rootName = root->string();
			if (cemuFolderName.empty())
				cemuFolderName = rootName;
			else if (cemuFolderName != rootName)
			{
				cemuLog_log(LogType::Force, "Rift update archive has multiple root entries");
				zip_close(za);
				return false;
			}

			const fs::path fname = targetpath / relativePath;
			if (sb.name[len - 1] == '/' || sb.name[len - 1] == '\\')
			{
				try
				{
					create_directories(fname);
				}
				catch (const std::exception& ex)
				{
					SystemException sys(ex);
					cemuLog_log(LogType::Force, "can't create folder \"{}\" for update: {}", sb.name, sys.what());
				}
				continue;
			}

			if (std::distance(relativePath.begin(), relativePath.end()) < 2)
			{
				zip_close(za);
				return false;
			}
			create_directories(fname.parent_path());

			auto* zf = zip_fopen_index(za, i, 0);
			if (!zf)
			{
				cemuLog_log(LogType::Force, "can't open zip file \"{}\"", sb.name);
				zip_close(za);
				return false;
			}

			std::vector<char> buffer(sb.size);
			const auto read = zip_fread(zf, buffer.data(), sb.size);
			if (read != (sint64)sb.size)
			{
				cemuLog_log(LogType::Force, "could only read 0x{:x} of 0x{:x} bytes from zip file \"{}\"", read, sb.size, sb.name);
				zip_fclose(zf);
				zip_close(za);
				return false;
			}

			auto* file = fopen(fname.string().c_str(), "wb");
			if (file == nullptr)
			{
				cemuLog_log(LogType::Force, "can't create update file \"{}\"", sb.name);
				zip_fclose(zf);
				zip_close(za);
				return false;
			}

			const size_t written = fwrite(buffer.data(), 1, buffer.size(), file);
			fflush(file);
			fclose(file);

			zip_fclose(zf);
			if (written != buffer.size())
			{
				zip_close(za);
				return false;
			}

			if ((i / 10) * 10 == i)
			{
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(i);
				wxQueueEvent(this, event);
			}
		}
	}

	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt(m_gaugeMaxValue);
	wxQueueEvent(this, event);

	zip_close(za);

	return true;
}

void CemuUpdateWindow::WorkerThread()
{
	const auto tmppath = fs::temp_directory_path() / L"rift_update";
	std::error_code ec;
	if (exists(tmppath))
		remove_all(tmppath, ec);

	while (true)
	{
		std::unique_lock lock(m_mutex);
		while (m_order == WorkerOrder::Idle)
			m_condition.wait_for(lock, std::chrono::milliseconds(125));

		if (m_order == WorkerOrder::Exit)
			break;

		try
		{
			if (m_order == WorkerOrder::CheckVersion)
			{
				auto* event = new wxCommandEvent(wxEVT_RESULT);
				const auto queryResult = QueryUpdateInfo(m_downloadUrl, m_changelogUrl, m_latestTag);
				if (queryResult == UpdateQueryResult::Available)
					event->SetInt((int)Result::UpdateAvailable);
				else if (queryResult == UpdateQueryResult::Ahead)
					event->SetInt((int)Result::Ahead);
				else if (queryResult == UpdateQueryResult::Current)
					event->SetInt((int)Result::NoUpdateAvailable);
				else
					event->SetInt((int)Result::CheckError);

				wxQueueEvent(this, event);
			}
			else if (m_order == WorkerOrder::UpdateVersion)
			{
				const std::string url = m_downloadUrl;
				if (!exists(tmppath))
					create_directory(tmppath);

#if BOOST_OS_WINDOWS
				const auto update_file = tmppath / L"update.zip";
#elif BOOST_OS_LINUX
				const auto update_file = tmppath / L"Cemu.AppImage";
#elif BOOST_OS_MACOS
				const auto update_file = tmppath / L"cemu.dmg";
#endif	
				if (DownloadCemuZip(url, update_file))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloaded);
					wxQueueEvent(this, event);
				}
				else
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloadError);
					wxQueueEvent(this, event);
					m_order = WorkerOrder::Idle;
					continue;
				}
				if (m_order == WorkerOrder::Exit)
					break;

				std::string cemuFolderName;
#if BOOST_OS_WINDOWS
				if (!ExtractUpdate(update_file, tmppath, cemuFolderName))
				{
					cemuLog_log(LogType::Force, "Extracting the Rift update failed");
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);
					continue;
				}
				if (cemuFolderName.empty())
				{
					cemuLog_log(LogType::Force, "Rift folder not found in the update archive");
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);
					continue;
				}
#endif
				const auto expected_path = tmppath / cemuFolderName;
				if (exists(expected_path))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractSuccess);
					wxQueueEvent(this, event);
				}
				else
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);

					if (exists(tmppath))
					{
						try
						{
							fs::remove_all(tmppath);
						}
						catch (const std::exception& ex)
						{
							SystemException sys(ex);
							cemuLog_log(LogType::Force, "can't remove extracted tmp files: {}", sys.what());
						}
					}

					continue;
				}

				if (m_order == WorkerOrder::Exit)
					break;

				fs::path exePath = ActiveSettings::GetExecutablePath();
#if BOOST_OS_WINDOWS
				const auto exec = ActiveSettings::GetExecutablePath();
				const auto stagedExecutable = expected_path / exec.filename();
				if (!fs::is_regular_file(stagedExecutable))
				{
					cemuLog_log(LogType::Force, "Rift update archive does not contain {}", _pathToUtf8(exec.filename()));
					auto* resultEvent = new wxCommandEvent(wxEVT_RESULT);
					resultEvent->SetInt((int)Result::Error);
					wxQueueEvent(this, resultEvent);
					continue;
				}

				const auto backupExecutable = fs::path(exec).replace_extension("exe.backup");
#elif BOOST_OS_LINUX
				const char* appimage_path = std::getenv("APPIMAGE");
				const auto target_exe = fs::path(appimage_path).replace_extension("AppImage.backup");
				const char* filePath = update_file.c_str();
				mode_t permissions = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
				fs::rename(appimage_path, target_exe);
				m_restartFile = appimage_path;
				chmod(filePath, permissions);
				wxString wxAppPath = wxString::FromUTF8(appimage_path);
				wxCopyFile (wxT("/tmp/rift_update/Cemu.AppImage"), wxAppPath);
#endif
#if BOOST_OS_WINDOWS
				int counter = 0;
				bool applySucceeded = true;
				try
				{
					if (fs::exists(backupExecutable))
						fs::remove(backupExecutable);
					fs::rename(exec, backupExecutable);

					for (const auto& it : fs::recursive_directory_iterator(expected_path))
					{
						const auto relativePath = fs::relative(it.path(), expected_path);
						const auto targetFile = exePath.parent_path() / relativePath;
						if (is_directory(it))
							fs::create_directories(targetFile);
						else
							fs::copy_file(it.path(), targetFile, fs::copy_options::overwrite_existing);

						if ((counter++ % 10) == 0)
						{
							auto* event = new wxCommandEvent(wxEVT_PROGRESS);
							event->SetInt(counter);
							wxQueueEvent(this, event);
						}
					}
				}
				catch (const std::exception& ex)
				{
					SystemException sys(ex);
					cemuLog_log(LogType::Force, "Applying the Rift update failed: {}", sys.what());
					applySucceeded = false;
				}

				if (!applySucceeded)
				{
					if (fs::exists(exec))
						fs::remove(exec);
					if (fs::exists(backupExecutable))
						fs::rename(backupExecutable, exec);
					auto* resultEvent = new wxCommandEvent(wxEVT_RESULT);
					resultEvent->SetInt((int)Result::Error);
					wxQueueEvent(this, resultEvent);
					continue;
				}
				m_restartFile = exec;
#endif
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(m_gaugeMaxValue);
				wxQueueEvent(this, event);

				auto* result_event = new wxCommandEvent(wxEVT_RESULT);
				result_event->SetInt((int)Result::Success);
				wxQueueEvent(this, result_event);
			}
		}
		catch (const std::exception& ex)
		{
			SystemException sys(ex);
			cemuLog_log(LogType::Force, "update error: {}", sys.what());

			if (exists(tmppath))
				remove_all(tmppath, ec);

			auto* result_event = new wxCommandEvent(wxEVT_RESULT);
			result_event->SetInt((int)Result::Error);
			wxQueueEvent(this, result_event);
		}

		m_order = WorkerOrder::Idle;
	}
}

void CemuUpdateWindow::OnClose(wxCloseEvent& event)
{
	event.Skip();

#if BOOST_OS_WINDOWS
	if (m_restartRequired && !m_restartFile.empty() && fs::exists(m_restartFile))
	{
		PROCESS_INFORMATION pi{};
		STARTUPINFO si{};
		si.cb = sizeof(si);

		std::wstring cmdline = GetCommandLineW();
		const auto index = cmdline.find('"', 1);
		cemu_assert_debug(index != std::wstring::npos);
		cmdline = L"\"" + m_restartFile.wstring() + L"\"" + cmdline.substr(index + 1);

		HANDLE lock = CreateMutex(nullptr, TRUE, L"Global\\cemu_update_lock");
		CreateProcess(nullptr, (wchar_t*)cmdline.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);

		exit(0);
	}
#elif BOOST_OS_LINUX
	if (m_restartRequired && !m_restartFile.empty() && fs::exists(m_restartFile))
	{
		const char* appimage_path = std::getenv("APPIMAGE");
		execlp(appimage_path, appimage_path, (char *)NULL);

		exit(0);
	}
#elif BOOST_OS_MACOS
	if (m_restartRequired)
	{
	    const auto tmppath = fs::temp_directory_path() / L"cemu_update/Cemu.dmg";
	    fs::path exePath = ActiveSettings::GetExecutablePath().parent_path();
	    const auto apppath = exePath / L"update.sh";
	    execlp("sh", "sh", apppath.c_str(), NULL);
        
        exit(0);
	}	
#endif
}


void CemuUpdateWindow::OnResult(wxCommandEvent& event)
{
	switch ((Result)event.GetInt())
	{
	case Result::NoUpdateAvailable:
		m_cancelButton->SetLabel(_("Exit"));
		m_text->SetLabel(_("Rift Iteration ") + wxString::FromUTF8(RiftVersion::Iteration) + _(" is current."));
		m_gauge->SetValue(100);
		break;
	case Result::UpdateAvailable:
	{
		if (!m_changelogUrl.empty())
		{
			m_changelog->SetURL(m_changelogUrl);
			m_changelog->Show();
		}
		else
			m_changelog->Hide();

		m_updateButton->Show();

		m_text->SetLabel(_("Rift ") + wxString::FromUTF8(m_latestTag) + _(" is available."));
		m_cancelButton->SetLabel(_("Exit"));
		break;
	}
	case Result::UpdateDownloaded:
		m_text->SetLabel(_("Extracting update..."));
		m_gauge->SetValue(0);
		break;
	case Result::UpdateDownloadError:
		m_updateButton->Enable();
		m_text->SetLabel(_("Couldn't download the update!"));
		break;
	case Result::ExtractSuccess:
		m_text->SetLabel(_("Applying update..."));
		m_gauge->SetValue(0);
		m_cancelButton->Disable();
		break;
	case Result::ExtractError:
		m_updateButton->Enable();
		m_cancelButton->Enable();
		m_text->SetLabel(_("Extracting failed!"));
		break;
	case Result::Success:
		m_cancelButton->Enable();
		m_updateButton->Hide();

		m_text->SetLabel(_("Update installed. Restart Rift to finish."));
		m_cancelButton->SetLabel(_("Restart"));
		m_restartRequired = true;
		break;
	case Result::Ahead:
		m_updateButton->Hide();
		m_cancelButton->SetLabel(_("Exit"));
		m_text->SetLabel(_("WOAH, WHAT ARE YOU DOING? This Rift build is newer than GitHub."));
		m_gauge->SetValue(100);
		break;
	case Result::CheckError:
		m_updateButton->Hide();
		m_cancelButton->SetLabel(_("Exit"));
		m_text->SetLabel(_("Couldn't check GitHub. Check your connection and try again."));
		break;
	case Result::Error:
		m_updateButton->Enable();
		m_cancelButton->Enable();
		m_text->SetLabel(_("The update could not be installed."));
		break;
	}
}

void CemuUpdateWindow::OnGaugeUpdate(wxCommandEvent& event)
{
	const int total_size = m_gaugeMaxValue > 0 ? m_gaugeMaxValue : 10000000;
	m_gauge->SetValue((event.GetInt() * 100) / total_size);
}

void CemuUpdateWindow::OnUpdateButton(const wxCommandEvent& event)
{
	std::unique_lock lock(m_mutex);
	m_order = WorkerOrder::UpdateVersion;

	m_condition.notify_all();

	m_updateButton->Disable();

	m_text->SetLabel(_("Downloading update..."));
}

void CemuUpdateWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}
