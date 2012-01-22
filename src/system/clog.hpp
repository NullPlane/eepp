#ifndef EECLOG_H
#define EECLOG_H

#include "base.hpp"
#include "tsingleton.hpp"
#include "ciostreamfile.hpp"

namespace EE { namespace System {

class EE_API cLog {
	SINGLETON_DECLARE_HEADERS(cLog)

	public:
		void Save(const std::string& filepath = "" );

		void Write(const std::string& Text, const bool& newLine = true);

		void Writef( const char* format, ... );

		std::string Buffer() const;

		const bool& ConsoleOutput() const;

		void ConsoleOutput( const bool& output );

		const bool& LiveWrite() const;

		void LiveWrite( const bool& lw );

		~cLog();
	protected:
		cLog();
	private:
		std::string		mData;
		std::string		mFilePath;
		bool			mSave;
		bool			mConsoleOutput;
		bool			mLiveWrite;
		cIOStreamFile *	mFS;

		void openfs();
};

}}
#endif
