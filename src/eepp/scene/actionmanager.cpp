#include <algorithm>
#include <eepp/core.hpp>
#include <eepp/scene/action.hpp>
#include <eepp/scene/actionmanager.hpp>
#include <eepp/system/lock.hpp>

namespace EE { namespace Scene {

ActionManager* ActionManager::New() {
	return eeNew( ActionManager, () );
}

ActionManager::ActionManager() : mUpdating( false ) {}

ActionManager::~ActionManager() {
	clear();
}

void ActionManager::addAction( Action* action ) {
	bool found = ( std::find( mActions.begin(), mActions.end(), action ) != mActions.end() );

	if ( !found ) {
		mActions.push_back( action );
	}
}

Action* ActionManager::getActionByTag( const Uint32& tag ) {
	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		if ( action->getTag() == tag )
			return action;
	}

	return NULL;
}

std::vector<Action*> ActionManager::getActionsFromTarget( Node* target ) {
	std::vector<Action*> actions;

	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		if ( action->getTarget() == target )
			actions.push_back( action );
	}

	return actions;
}

std::vector<Action*> ActionManager::getActionsByTagFromTarget( Node* target, const Uint32& tag ) {
	std::vector<Action*> actions;

	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		if ( action->getTarget() == target && action->getTag() == tag )
			actions.push_back( action );
	}

	return actions;
}

void ActionManager::removeActionByTag( const Uint32& tag ) {
	removeAction( getActionByTag( tag ) );
}

void ActionManager::removeActionsByTagFromTarget( Node* target, const Uint32& tag ) {
	std::vector<Action*> removeList;

	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		if ( action->getTarget() == target && action->getTag() == tag ) {
			removeList.push_back( *it );
		}
	}

	for ( auto it = removeList.begin(); it != removeList.end(); ++it )
		removeAction( ( *it ) );
}

void ActionManager::update( const Time& time ) {
	if ( isEmpty() )
		return;

	std::vector<Action*> removeList;

	mUpdating = true;

	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		action->update( time );

		if ( action->isDone() ) {
			action->sendEvent( Action::ActionType::OnDone );

			removeList.push_back( action );
		}
	}

	mUpdating = false;

	for ( auto it = mActionsRemoveList.begin(); it != mActionsRemoveList.end(); ++it )
		removeAction( ( *it ) );

	mActionsRemoveList.clear();

	for ( auto it = removeList.begin(); it != removeList.end(); ++it )
		removeAction( ( *it ) );
}

std::size_t ActionManager::count() const {
	return mActions.size();
}

bool ActionManager::isEmpty() const {
	return mActions.empty();
}

void ActionManager::clear() {
	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		eeSAFE_DELETE( action );
	}

	mActions.clear();
}

void ActionManager::removeAction( Action* action ) {
	if ( NULL != action ) {
		if ( !mUpdating ) {
			bool found = std::find( mActions.begin(), mActions.end(), action ) != mActions.end();

			if ( found ) {
				mActions.remove( action );

				eeSAFE_DELETE( action );
			}
		} else {
			mActionsRemoveList.push_back( action );
		}
	}
}

void ActionManager::removeActions( const std::vector<Action*>& actions ) {
	for ( auto& action : actions ) {
		removeAction( action );
	}
}

void ActionManager::removeAllActionsFromTarget( Node* target ) {
	std::vector<Action*> removeList;

	for ( auto it = mActions.begin(); it != mActions.end(); ++it ) {
		Action* action = ( *it );

		if ( action->getTarget() == target ) {
			removeList.push_back( *it );
		}
	}

	for ( auto it = removeList.begin(); it != removeList.end(); ++it )
		removeAction( ( *it ) );
}

}} // namespace EE::Scene
