#include <eepp/ui/css/stylesheetselectorrule.hpp>
#include <eepp/ui/css/stylesheetelement.hpp>
#include <algorithm>

namespace EE { namespace UI { namespace CSS {

static const char * StatePseudoClasses[] = {
	"normal",
	"focus",
	"selected",
	"hover",
	"pressed",
	"selectedhover",
	"selectedpressed",
	"disabled"
};

static bool isPseudoClassState( const std::string& pseudoClass ) {
	for ( Uint32 i = 0; i < eeARRAY_SIZE(StatePseudoClasses); i++ ) {
		if ( pseudoClass == StatePseudoClasses[i] )
			return true;
	}

	return false;
}

static const char * StructuralPseudoClasses[] = {
	"root",
	"nth-child",
	"nth-last-child",
	"nth-of-type",
	"nth-last-of-type",
	"nth-child",
	"nth-last-child",
	"first-of-type",
	"last-of-type",
	"only-child",
	"only-of-type",
	"empty"
};

static bool isStructuralPseudoClass( const std::string& pseudoClass ) {
	for ( Uint32 i = 0; i < eeARRAY_SIZE(StructuralPseudoClasses); i++ ) {
		if ( String::startsWith( StructuralPseudoClasses[i], pseudoClass ) )
			return true;
	}

	return false;
}

static void splitSelectorPseudoClass( const std::string& selector, std::string& realSelector, std::string& realPseudoClass ) {
	if ( !selector.empty() ) {
		bool lastWasColon = false;

		for ( int i = (Int32)selector.size() - 1; i >= 0; i-- ) {
			char curChar = selector[i];

			if ( lastWasColon ) {
				if ( StyleSheetSelectorRule::PSEUDO_CLASS == curChar ) {
					// no pseudo class
					realSelector = selector;
				} else {
					if ( i+2 <= (int)selector.size() ) {
						realSelector = selector.substr(0,i+1);
						realPseudoClass = selector.substr(i+2);
					} else {
						realSelector = selector;
					}
				}

				return;
			} else if ( StyleSheetSelectorRule::PSEUDO_CLASS == curChar ) {
				lastWasColon = true;
			}
		}

		if ( lastWasColon ) {
			if ( selector.size() > 1 )
				realPseudoClass = selector.substr(1);
		} else {
			realSelector = selector;
		}
	}
}


StyleSheetSelectorRule::StyleSheetSelectorRule( const std::string& selectorFragment, PatternMatch patternMatch ) :
	mSpecificity(0),
	mPatternMatch( patternMatch ),
	mRequirementFlags(0)
{
	parseFragment( selectorFragment );
}

void StyleSheetSelectorRule::pushSelectorTypeIdentifier( TypeIdentifier selectorTypeIdentifier, std::string name ) {
	switch ( selectorTypeIdentifier ) {
		case GLOBAL:
			mTagName = name;
			mSpecificity += SpecificityGlobal;
		case TAG:
			mTagName = name;
			mSpecificity += SpecificityTag;
			break;
		case CLASS:
			mClasses.push_back( name );
			mSpecificity += SpecificityClass;
			break;
		case ID:
			mId = name;
			mSpecificity += SpecificityId;
			break;
		default:
			break;
	}
}

void StyleSheetSelectorRule::parseFragment( const std::string& selectorFragment ) {
	std::string selector = selectorFragment;
	std::string realSelector = "";
	std::string pseudoClass = "";

	do {
		pseudoClass.clear();
		realSelector.clear();

		splitSelectorPseudoClass( selector, realSelector, pseudoClass );

		if ( !pseudoClass.empty() ) {
			if ( isPseudoClassState( pseudoClass ) ) {
				mPseudoClasses.push_back( pseudoClass );
			} else if ( isStructuralPseudoClass( pseudoClass ) ) {
				mStructuralPseudoClasses.push_back( pseudoClass );
			}

			selector = realSelector;
		}
	} while ( !pseudoClass.empty() );

	TypeIdentifier curSelectorType = TAG;
	std::string buffer;

	for ( auto charIt = selector.begin(); charIt != selector.end(); ++charIt ) {
		char curChar = *charIt;

		switch ( curChar ) {
			case CLASS:
			{
				if ( !buffer.empty() ) {
					pushSelectorTypeIdentifier( curSelectorType, buffer );
					buffer.clear();
				}

				curSelectorType = CLASS;

				break;
			}
			case ID:
			{
				if ( !buffer.empty() ) {
					pushSelectorTypeIdentifier( curSelectorType, buffer );
					buffer.clear();
				}

				curSelectorType = ID;

				break;
			}
			default:
			{
				buffer += curChar;
				break;
			}
		}
	}

	if ( !buffer.empty() ) {
		if ( buffer.size() == 1 && buffer[0] == GLOBAL )
			curSelectorType = GLOBAL;

		pushSelectorTypeIdentifier( curSelectorType, buffer );
	}

	if ( !mTagName.empty() )
		mRequirementFlags |= TagName;

	if ( !mId.empty() )
		mRequirementFlags |= Id;

	if ( !mClasses.empty() )
		mRequirementFlags |= Class;

	if ( !mPseudoClasses.empty() ) {
		mRequirementFlags |= PseudoClass;

		for ( auto it = mPseudoClasses.begin(); it != mPseudoClasses.end(); ++it ) {
			mSpecificity += SpecificityPseudoClass;
		}
	}
}

bool StyleSheetSelectorRule::hasClass( const std::string& cls ) const {
	return std::find(mClasses.begin(), mClasses.end(), cls) != mClasses.end();
}

bool StyleSheetSelectorRule::hasPseudoClasses() const {
	return !mPseudoClasses.empty();
}

bool StyleSheetSelectorRule::hasPseudoClass( const std::string& cls ) const {
	return std::find(mPseudoClasses.begin(), mPseudoClasses.end(), cls) != mPseudoClasses.end();
}

const std::vector<std::string> &StyleSheetSelectorRule::getPseudoClasses() const {
	return mPseudoClasses;
}

bool StyleSheetSelectorRule::hasStructuralPseudoClasses() const {
	return !mStructuralPseudoClasses.empty();
}

const std::vector<std::string> &StyleSheetSelectorRule::getStructuralPseudoClasses() const {
	return mStructuralPseudoClasses;
}

bool StyleSheetSelectorRule::hasStructuralPseudoClass( const std::string& cls ) const {
	return std::find(mStructuralPseudoClasses.begin(), mStructuralPseudoClasses.end(), cls) != mStructuralPseudoClasses.end();
}

bool StyleSheetSelectorRule::matches( StyleSheetElement * element, const bool& applyPseudo ) const {
	Uint32 flags = 0;

	if ( mTagName == "*" )
		return true;

	if ( !mTagName.empty() && !element->getStyleSheetTag().empty() && mTagName == element->getStyleSheetTag() ) {
		flags |= TagName;
	}

	if ( !mId.empty() && !element->getStyleSheetId().empty() && mId == element->getStyleSheetId() ) {
		flags |= Id;
	}

	if ( !mClasses.empty() && !element->getStyleSheetClasses().empty() ) {
		bool hasClasses = true;
		for ( auto cit = element->getStyleSheetClasses().begin(); cit != element->getStyleSheetClasses().end(); ++cit ) {
			if ( !hasClass( *cit ) ) {
				hasClasses = false;
				break;
			}
		}

		if ( hasClasses ) {
			flags |= Class;
		}
	}

	if ( applyPseudo ) {
		if ( mPseudoClasses.empty() && !element->getStyleSheetPseudoClasses().empty() ) {
			flags |= PseudoClass;
		} else if ( !mPseudoClasses.empty() && !element->getStyleSheetPseudoClasses().empty() ) {
			bool hasPseudoClasses = false;
			const std::vector<std::string>& elPseudoClasses = element->getStyleSheetPseudoClasses();

			for ( auto cit = elPseudoClasses.begin(); cit != elPseudoClasses.end(); ++cit ) {
				if ( hasPseudoClass( *cit ) ) {
					hasPseudoClasses = true;
					break;
				}
			}

			if ( hasPseudoClasses ) {
				flags |= PseudoClass;
			}
		}

		return mRequirementFlags == flags;
	}

	return ( mRequirementFlags & ~PseudoClass ) == flags;
}

}}}