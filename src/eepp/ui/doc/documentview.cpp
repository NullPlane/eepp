#include <eepp/graphics/text.hpp>
#include <eepp/system/luapattern.hpp>
#include <eepp/system/scopedop.hpp>
#include <eepp/ui/doc/documentview.hpp>

namespace EE { namespace UI { namespace Doc {

LineWrapMode DocumentView::toLineWrapMode( std::string mode ) {
	String::toLowerInPlace( mode );
	if ( mode == "word" )
		return LineWrapMode::Word;
	if ( mode == "letter" )
		return LineWrapMode::Letter;
	return LineWrapMode::NoWrap;
}

std::string DocumentView::fromLineWrapMode( LineWrapMode mode ) {
	switch ( mode ) {
		case LineWrapMode::Letter:
			return "letter";
		case LineWrapMode::Word:
			return "word";
		case LineWrapMode::NoWrap:
		default:
			return "nowrap";
	}
}

LineWrapType DocumentView::toLineWrapType( std::string type ) {
	String::toLowerInPlace( type );
	if ( "line_breaking_column" == type )
		return LineWrapType::LineBreakingColumn;
	return LineWrapType::Viewport;
}

std::string DocumentView::fromLineWrapType( LineWrapType type ) {
	switch ( type ) {
		case LineWrapType::LineBreakingColumn:
			return "line_breaking_column";
		case LineWrapType::Viewport:
		default:
			return "viewport";
	}
}

Float DocumentView::computeOffsets( const String::View& string, const FontStyleConfig& fontStyle,
									Uint32 tabWidth ) {

	static const String sepSpaces = " \t\n\v\f\r";
	auto nonIndentPos = string.find_first_not_of( sepSpaces.data() );
	if ( nonIndentPos != String::View::npos )
		return Text::getTextWidth( string.substr( 0, nonIndentPos ), fontStyle, tabWidth );
	return 0.f;
}

DocumentView::LineWrapInfo DocumentView::computeLineBreaks( const String::View& string,
															const FontStyleConfig& fontStyle,
															Float maxWidth, LineWrapMode mode,
															bool keepIndentation,
															Uint32 tabWidth ) {
	LineWrapInfo info;
	info.wraps.push_back( 0 );
	if ( string.empty() || nullptr == fontStyle.Font || mode == LineWrapMode::NoWrap )
		return info;

	if ( keepIndentation )
		info.paddingStart = computeOffsets( string, fontStyle, tabWidth );

	Float xoffset = 0.f;
	Float lastWidth = 0.f;
	bool bold = ( fontStyle.Style & Text::Style::Bold ) != 0;
	bool italic = ( fontStyle.Style & Text::Style::Italic ) != 0;
	bool isMonospace = fontStyle.Font->isMonospace();
	Float outlineThickness = fontStyle.OutlineThickness;

	size_t lastSpace = 0;
	Uint32 prevChar = 0;

	Float hspace = static_cast<Float>(
		fontStyle.Font->getGlyph( L' ', fontStyle.CharacterSize, bold, italic, outlineThickness )
			.advance );
	size_t idx = 0;

	for ( const auto& curChar : string ) {
		Float w = !isMonospace ? fontStyle.Font
									 ->getGlyph( curChar, fontStyle.CharacterSize, bold, italic,
												 outlineThickness )
									 .advance
							   : hspace;

		if ( curChar == '\t' )
			w = hspace * tabWidth;
		else if ( ( curChar ) == '\r' )
			w = 0;

		if ( !isMonospace && curChar != '\r' ) {
			w += fontStyle.Font->getKerning( prevChar, curChar, fontStyle.CharacterSize, bold,
											 italic, outlineThickness );
			prevChar = curChar;
		}

		xoffset += w;

		if ( xoffset > maxWidth ) {
			if ( mode == LineWrapMode::Word && lastSpace ) {
				info.wraps.push_back( lastSpace + 1 );
				xoffset = w + info.paddingStart + ( xoffset - lastWidth );
			} else {
				info.wraps.push_back( idx );
				xoffset = w + info.paddingStart;
			}
			lastSpace = 0;
		} else if ( curChar == ' ' || curChar == '.' || curChar == '-' || curChar == ',' ) {
			lastSpace = idx;
			lastWidth = xoffset;
		}

		idx++;
	}

	return info;
}

DocumentView::LineWrapInfo DocumentView::computeLineBreaks( const String& string,
															const FontStyleConfig& fontStyle,
															Float maxWidth, LineWrapMode mode,
															bool keepIndentation,
															Uint32 tabWidth ) {
	return computeLineBreaks( string.view(), fontStyle, maxWidth, mode, keepIndentation, tabWidth );
}

DocumentView::LineWrapInfo DocumentView::computeLineBreaks( const TextDocument& doc, size_t line,
															const FontStyleConfig& fontStyle,
															Float maxWidth, LineWrapMode mode,
															bool keepIndentation,
															Uint32 tabWidth ) {
	const auto& text = doc.line( line ).getText();
	return computeLineBreaks( text.substr( 0, text.size() - 1 ), fontStyle, maxWidth, mode,
							  keepIndentation, tabWidth );
}

DocumentView::DocumentView( std::shared_ptr<TextDocument> doc, FontStyleConfig fontStyle,
							Config config ) :
	mDoc( std::move( doc ) ),
	mFontStyle( std::move( fontStyle ) ),
	mConfig( std::move( config ) ) {}

bool DocumentView::isWrapEnabled() const {
	return mConfig.mode != LineWrapMode::NoWrap;
}

void DocumentView::setMaxWidth( Float maxWidth, bool forceReconstructBreaks ) {
	if ( maxWidth != mMaxWidth ) {
		mMaxWidth = maxWidth;
		invalidateCache();
	} else if ( forceReconstructBreaks || mPendingReconstruction ) {
		invalidateCache();
	}
}

void DocumentView::setFontStyle( FontStyleConfig fontStyle ) {
	if ( fontStyle != mFontStyle ) {
		mFontStyle = std::move( fontStyle );
		invalidateCache();
	}
}

void DocumentView::setLineWrapMode( LineWrapMode mode ) {
	if ( mode != mConfig.mode ) {
		mConfig.mode = mode;
		invalidateCache();
	}
}

TextPosition DocumentView::getVisibleIndexPosition( VisibleIndex visibleIndex ) const {
	if ( isOneToOne() )
		return { static_cast<Int64>( visibleIndex ), 0 };
	return mVisibleLines[eeclamp( static_cast<Int64>( visibleIndex ), 0ll,
								  eemax( static_cast<Int64>( mVisibleLines.size() ) - 1, 0ll ) )];
}

Float DocumentView::getLinePadding( Int64 docIdx ) const {
	if ( isOneToOne() || mVisibleLinesOffset.empty() )
		return 0;
	return mVisibleLinesOffset[eeclamp(
		docIdx, 0ll, eemax( static_cast<Int64>( mVisibleLinesOffset.size() ) - 1, 0ll ) )];
}

void DocumentView::setConfig( Config config ) {
	if ( config != mConfig ) {
		mConfig = std::move( config );
		invalidateCache();
	}
}

void DocumentView::invalidateCache() {
	if ( 0 == mMaxWidth || !mDoc )
		return;

	if ( mDoc->isLoading() ) {
		mPendingReconstruction = !isOneToOne();
		return;
	}

	BoolScopedOp op( mUnderConstruction, true );

	mVisibleLines.clear();
	mDocLineToVisibleIndex.clear();
	mVisibleLinesOffset.clear();

	bool wrap = mConfig.mode != LineWrapMode::NoWrap;
	Int64 linesCount = mDoc->linesCount();
	mVisibleLines.reserve( linesCount );
	mVisibleLinesOffset.reserve( linesCount );
	mDocLineToVisibleIndex.reserve( linesCount );

	for ( auto i = 0; i < linesCount; i++ ) {
		if ( isFolded( i ) ) {
			mVisibleLinesOffset.emplace_back(
				wrap ? computeOffsets( mDoc->line( i ).getText().view(), mFontStyle,
									   mConfig.tabWidth )
					 : 0 );
			mDocLineToVisibleIndex.push_back( static_cast<Int64>( VisibleIndex::invalid ) );
		} else {
			auto lb = wrap ? computeLineBreaks( *mDoc, i, mFontStyle, mMaxWidth, mConfig.mode,
												mConfig.keepIndentation, mConfig.tabWidth )
						   : LineWrapInfo{ { 0 }, 0.f };
			mVisibleLinesOffset.emplace_back( lb.paddingStart );
			bool first = true;
			for ( const auto& col : lb.wraps ) {
				mVisibleLines.emplace_back( i, col );
				if ( first ) {
					mDocLineToVisibleIndex.emplace_back( mVisibleLines.size() - 1 );
					first = false;
				}
			}
		}
	}

	eeASSERT( static_cast<Int64>( mDocLineToVisibleIndex.size() ) == linesCount );

	mPendingReconstruction = false;
}

VisibleIndex DocumentView::toVisibleIndex( Int64 docIdx, bool retLast ) const {
	// eeASSERT( isLineVisible( docIdx ) );
	if ( isOneToOne() )
		return static_cast<VisibleIndex>( docIdx );
	auto idx = mDocLineToVisibleIndex[eeclamp(
		docIdx, 0ll, static_cast<Int64>( mDocLineToVisibleIndex.size() - 1 ) )];
	if ( retLast ) {
		Int64 lastOfLine = mVisibleLines[idx].line();
		Int64 visibleLinesCount = mVisibleLines.size();
		for ( auto i = idx + 1; i < visibleLinesCount; i++ ) {
			if ( mVisibleLines[i].line() == lastOfLine )
				idx = i;
			else
				break;
		}
	}
	return static_cast<VisibleIndex>( idx );
}

bool DocumentView::isWrappedLine( Int64 docIdx ) const {
	if ( isWrapEnabled() ) {
		Int64 visibleIndex = static_cast<Int64>( toVisibleIndex( docIdx ) );
		return visibleIndex + 1 < static_cast<Int64>( mVisibleLines.size() ) &&
			   mVisibleLines[visibleIndex].line() == mVisibleLines[visibleIndex + 1].line();
	}
	return false;
}

DocumentView::VisibleLineInfo DocumentView::getVisibleLineInfo( Int64 docIdx ) const {
	eeASSERT( isLineVisible( docIdx ) );
	VisibleLineInfo line;
	if ( isOneToOne() ) {
		line.visualLines.push_back( { docIdx, 0 } );
		line.visibleIndex = static_cast<VisibleIndex>( docIdx );
		return line;
	}
	Int64 fromIdx = static_cast<Int64>( toVisibleIndex( docIdx ) );
	Int64 toIdx = static_cast<Int64>( toVisibleIndex( docIdx, true ) );
	line.visualLines.reserve( toIdx - fromIdx + 1 );
	for ( Int64 i = fromIdx; i <= toIdx; i++ )
		line.visualLines.emplace_back( mVisibleLines[i] );
	line.visibleIndex = static_cast<VisibleIndex>( fromIdx );
	line.paddingStart = mVisibleLinesOffset[docIdx];
	return line;
}

DocumentView::VisibleLineRange DocumentView::getVisibleLineRange( const TextPosition& pos,
																  bool allowVisualLineEnd ) const {
	if ( isOneToOne() ) {
		DocumentView::VisibleLineRange info;
		info.visibleIndex = static_cast<VisibleIndex>( pos.line() );
		info.range = mDoc->getLineRange( pos.line() );
		return info;
	}
	Int64 fromIdx = static_cast<Int64>( toVisibleIndex( pos.line() ) );
	Int64 toIdx = static_cast<Int64>( toVisibleIndex( pos.line(), true ) );
	DocumentView::VisibleLineRange info;
	for ( Int64 i = fromIdx; i < toIdx; i++ ) {
		Int64 fromCol = mVisibleLines[i].column();
		Int64 toCol = i + 1 <= toIdx
						  ? mVisibleLines[i + 1].column() - ( allowVisualLineEnd ? 0 : 1 )
						  : mDoc->line( pos.line() ).size();
		if ( pos.column() >= fromCol && pos.column() <= toCol ) {
			info.visibleIndex = static_cast<VisibleIndex>( i );
			info.range = { { pos.line(), fromCol }, { pos.line(), toCol } };
			return info;
		}
	}
	eeASSERT( toIdx >= 0 );
	info.visibleIndex = static_cast<VisibleIndex>( toIdx );
	info.range = { { pos.line(), mVisibleLines[toIdx].column() },
				   mDoc->endOfLine( { pos.line(), 0ll } ) };
	return info;
}

TextRange DocumentView::getVisibleIndexRange( VisibleIndex visibleIndex ) const {
	if ( isOneToOne() )
		return mDoc->getLineRange( static_cast<Int64>( visibleIndex ) );
	Int64 idx = static_cast<Int64>( visibleIndex );
	auto start = getVisibleIndexPosition( visibleIndex );
	auto end = start;
	if ( idx + 1 < static_cast<Int64>( mVisibleLines.size() ) &&
		 mVisibleLines[idx + 1].line() == start.line() ) {
		end.setColumn( mVisibleLines[idx + 1].column() );
	} else {
		end.setColumn( mDoc->line( start.line() ).size() );
	}
	return { start, end };
}

std::shared_ptr<TextDocument> DocumentView::getDocument() const {
	return mDoc;
}

void DocumentView::setDocument( const std::shared_ptr<TextDocument>& doc ) {
	if ( mDoc != doc ) {
		mDoc = doc;
		invalidateCache();
	}
}

bool DocumentView::isPendingReconstruction() const {
	return mPendingReconstruction;
}

void DocumentView::setPendingReconstruction( bool pendingReconstruction ) {
	mPendingReconstruction = pendingReconstruction;
}

void DocumentView::clearCache() {
	mVisibleLines.clear();
	mDocLineToVisibleIndex.clear();
	mVisibleLinesOffset.clear();
}

void DocumentView::clear() {
	clearCache();
	mFoldingRegions.clear();
	mFoldedRegions.clear();
}

Float DocumentView::getLineYOffset( VisibleIndex visibleIndex, Float lineHeight ) const {
	return static_cast<Float>( visibleIndex ) * lineHeight;
}

Float DocumentView::getLineYOffset( Int64 docIdx, Float lineHeight ) const {
	eeASSERT( docIdx >= 0 && docIdx < static_cast<Int64>( mDoc->linesCount() ) );
	return static_cast<Float>( toVisibleIndex( docIdx ) ) * lineHeight;
}

bool DocumentView::isLineVisible( Int64 docIdx ) const {
	return mDocLineToVisibleIndex[docIdx] != static_cast<Int64>( VisibleIndex::invalid );
}

void DocumentView::updateCache( Int64 fromLine, Int64 toLine, Int64 numLines ) {
	if ( isOneToOne() )
		return;
	// Get affected visible range
	Int64 oldIdxFrom = static_cast<Int64>( toVisibleIndex( fromLine, false ) );
	Int64 oldIdxTo = static_cast<Int64>( toVisibleIndex( toLine, true ) );

	// Remove old visible lines
	mVisibleLines.erase( mVisibleLines.begin() + oldIdxFrom, mVisibleLines.begin() + oldIdxTo + 1 );

	// Remove old offsets
	mVisibleLinesOffset.erase( mVisibleLinesOffset.begin() + fromLine,
							   mVisibleLinesOffset.begin() + toLine + 1 );

	// Shift the line numbers
	if ( numLines != 0 ) {
		Int64 visibleLinesCount = mVisibleLines.size();
		for ( Int64 i = oldIdxFrom; i < visibleLinesCount; i++ )
			mVisibleLines[i].setLine( mVisibleLines[i].line() + numLines );

		shiftFoldingRegions( fromLine, numLines );
	}

	// Recompute line breaks
	auto netLines = toLine + numLines;
	auto idxOffset = oldIdxFrom;
	for ( auto i = fromLine; i <= netLines; i++ ) {
		if ( isFolded( i ) ) {
			mVisibleLinesOffset.insert(
				mVisibleLinesOffset.begin() + i,
				computeOffsets( mDoc->line( i ).getText().view(), mFontStyle, mConfig.tabWidth ) );
			mDocLineToVisibleIndex[i] = static_cast<Int64>( VisibleIndex::invalid );
		} else {
			auto lb = computeLineBreaks( *mDoc, i, mFontStyle, mMaxWidth, mConfig.mode,
										 mConfig.keepIndentation, mConfig.tabWidth );
			mVisibleLinesOffset.insert( mVisibleLinesOffset.begin() + i, lb.paddingStart );
			for ( const auto& col : lb.wraps ) {
				mVisibleLines.insert( mVisibleLines.begin() + idxOffset, { i, col } );
				idxOffset++;
			}
		}
	}

	// Recompute document line to visible index
	Int64 visibleLinesCount = mVisibleLines.size();
	mDocLineToVisibleIndex.resize( mDoc->linesCount() );
	Int64 previousLineIdx = mVisibleLines[oldIdxFrom].line();
	for ( Int64 visibleIdx = oldIdxFrom; visibleIdx < visibleLinesCount; visibleIdx++ ) {
		const auto& visibleLine = mVisibleLines[visibleIdx];
		if ( visibleLine.column() == 0 ) {
			// Non-contiguos lines means hidden lines
			if ( visibleLine.line() - previousLineIdx > 1 ) {
				for ( Int64 i = previousLineIdx + 1; i < visibleLine.line(); i++ )
					mDocLineToVisibleIndex[i] = static_cast<Int64>( VisibleIndex::invalid );
			}
			mDocLineToVisibleIndex[visibleLine.line()] = visibleIdx;
			previousLineIdx = visibleLine.line();
		}
	}

#ifdef EE_DEBUG
	auto visibleLines = mVisibleLines;
	auto docLineToVisibleIndex = mDocLineToVisibleIndex;
	auto visibleLinesOffset = mVisibleLinesOffset;

	invalidateCache();

	eeASSERT( visibleLines == mVisibleLines );
	eeASSERT( docLineToVisibleIndex == mDocLineToVisibleIndex );
	eeASSERT( visibleLinesOffset == mVisibleLinesOffset );
#endif
}

size_t DocumentView::getVisibleLinesCount() const {
	return isOneToOne() ? mDoc->linesCount() : mVisibleLines.size();
}

void DocumentView::addFoldRegion( TextRange region ) {
	region.normalize();
	mFoldingRegions[region.start().line()] = std::move( region );
}

bool DocumentView::isFoldingRegionInLine( Int64 docIdx ) {
	auto foldRegionIt = mFoldingRegions.find( docIdx );
	return foldRegionIt != mFoldingRegions.end();
}

void DocumentView::foldRegion( Int64 foldDocIdx ) {
	auto foldRegionIt = mFoldingRegions.find( foldDocIdx );
	if ( foldRegionIt == mFoldingRegions.end() )
		return;
	Int64 toDocIdx = foldRegionIt->second.end().line();
	changeVisibility( foldDocIdx, toDocIdx, false );
	bool foldWasEmpty = mFoldedRegions.empty();
	mFoldedRegions.push_back( foldRegionIt->second );
	std::sort( mFoldedRegions.begin(), mFoldedRegions.end() );
	if ( foldWasEmpty && mConfig.mode == LineWrapMode::NoWrap )
		invalidateCache();
}

void DocumentView::unfoldRegion( Int64 foldDocIdx ) {
	auto foldRegionIt = mFoldingRegions.find( foldDocIdx );
	if ( foldRegionIt == mFoldingRegions.end() )
		return;
	Int64 toDocIdx = foldRegionIt->second.end().line();
	changeVisibility( foldDocIdx, toDocIdx, true );
	removeFoldedRegion( foldRegionIt->second );
	if ( isOneToOne() )
		clearCache();
}

bool DocumentView::isOneToOne() const {
	return mConfig.mode == LineWrapMode::NoWrap && mFoldedRegions.empty();
}

void DocumentView::changeVisibility( Int64 fromDocIdx, Int64 toDocIdx, bool visible ) {
	if ( visible ) {
		auto it = std::lower_bound( mVisibleLines.begin(), mVisibleLines.end(),
									TextPosition{ fromDocIdx, 0 } );
		auto idxOffset = std::distance( mVisibleLines.begin(), it );
		for ( auto i = fromDocIdx; i <= toDocIdx; i++ ) {
			auto lb = isWrapEnabled()
						  ? computeLineBreaks( *mDoc, i, mFontStyle, mMaxWidth, mConfig.mode,
											   mConfig.keepIndentation, mConfig.tabWidth )
						  : LineWrapInfo{ { 0 }, 0 };
			mVisibleLinesOffset[i] = lb.paddingStart;
			for ( const auto& col : lb.wraps ) {
				mVisibleLines.insert( mVisibleLines.begin() + idxOffset, { i, col } );
				idxOffset++;
			}
		}
	} else {
		Int64 oldIdxFrom = static_cast<Int64>( toVisibleIndex( fromDocIdx ) );
		Int64 oldIdxTo = static_cast<Int64>( toVisibleIndex( toDocIdx, true ) );
		mVisibleLines.erase( mVisibleLines.begin() + oldIdxFrom,
							 mVisibleLines.begin() + oldIdxTo + 1 );
		for ( Int64 idx = fromDocIdx; idx <= toDocIdx; idx++ ) {
			mDocLineToVisibleIndex[idx] = static_cast<Int64>( VisibleIndex::invalid );
		}
		Int64 linesCount = mDoc->linesCount();
		Int64 idxOffset = oldIdxTo - oldIdxFrom + 1;
		for ( Int64 idx = toDocIdx + 1; idx < linesCount; idx++ )
			mDocLineToVisibleIndex[idx] -= idxOffset;
		eeASSERT( mDocLineToVisibleIndex.size() == mDoc->linesCount() );
	}
}

void DocumentView::removeFoldedRegion( const TextRange& region ) {
	auto found = std::find( mFoldedRegions.begin(), mFoldedRegions.end(), region );
	if ( found != mFoldedRegions.end() )
		mFoldedRegions.erase( found );
}

bool DocumentView::isFolded( Int64 docIdx ) const {
	return std::any_of(
		mFoldedRegions.begin(), mFoldedRegions.end(),
		[docIdx]( const TextRange& region ) { return region.containsLine( docIdx ); } );
}

void DocumentView::shiftFoldingRegions( Int64 fromLine, Int64 numLines ) {
	for ( auto& [_, region] : mFoldingRegions ) {
		if ( region.start().line() >= fromLine ) {
			region.start().setLine( region.start().line() + numLines );
			region.end().setLine( region.end().line() + numLines );
		}
	}
	for ( auto& region : mFoldedRegions ) {
		if ( region.start().line() >= fromLine ) {
			region.start().setLine( region.start().line() + numLines );
			region.end().setLine( region.end().line() + numLines );
		}
	}
}

}}} // namespace EE::UI::Doc
