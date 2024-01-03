#include <map>
#include <string>
#include <vector>

namespace ecode {

class Git {
  public:
	struct Blame {
		Blame( const std::string& error );

		Blame( std::string&& author, std::string&& authorEmail, std::string&& date,
			   std::string&& commitHash, std::string&& commitShortHash, std::string&& commitMessage,
			   std::size_t line );

		std::string author;
		std::string authorEmail;
		std::string date;
		std::string commitHash;
		std::string commitShortHash;
		std::string commitMessage;
		std::string error;
		std::size_t line{ 0 };
	};

	struct DiffFile {
		std::string file;
		int inserts{ 0 };
		int deletes{ 0 };

		bool operator==( const DiffFile& other ) const {
			return file == other.file && inserts == other.inserts && deletes == other.deletes;
		}
	};

	enum FileStatus {
		Unidentified,
		Modified = 'M',
		Added = 'A',
		Renamed = 'R',
		TypeChanged = 'T',
		UpdatedUnmerged = 'U',
		Deleted = 'D',
		Untracked = '?',
		ModifiedSubmodule = 'm',
	};

	struct Status {
		std::vector<DiffFile> modified;
		int totalInserts{ 0 };
		int totalDeletions{ 0 };
		std::map<std::string, FileStatus> files;

		bool operator==( const Status& other ) const {
			return totalInserts == other.totalInserts && totalDeletions == other.totalDeletions &&
				   modified == other.modified && files == other.files;
		}
	};

	Git( const std::string& projectDir = "", const std::string& gitPath = "" );

	void git( const std::string& args, const std::string& projectDir, std::string& buf ) const;

	void gitSubmodules( const std::string& args, const std::string& projectDir, std::string& buf );

	Blame blame( const std::string& filepath, std::size_t line ) const;

	std::string branch( const std::string& projectDir = "" );

	Status status( bool recurseSubmodules, const std::string& projectDir = "" );

	bool setProjectPath( const std::string& projectPath );

	const std::string& getGitPath() const;

	const std::string& getProjectPath() const;

	const std::string& getGitFolder() const;

  protected:
	std::string mGitPath;
	std::string mProjectPath;
	std::string mGitFolder;

	bool hasSubmodules( const std::string& projectDir );
};

} // namespace ecode
