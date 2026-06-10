using namespace QPI;
#include "qpi.h"

struct QFOMO : public ContractBase
{
    struct StateData
    {
        // empty for now
    };

    INITIALIZE()
    {
    }

    PUBLIC_PROCEDURE(PlaceBid)
    {
    }

    PUBLIC_PROCEDURE(Claim)
    {
    }

    PUBLIC_FUNCTION(GetGame)
    {
    }

    END_TICK()
    {
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_PROCEDURE(PlaceBid, 1);
        REGISTER_USER_PROCEDURE(Claim, 2);

        REGISTER_USER_FUNCTION(GetGame, 1);
    }
};