#include<iostream>
#include<vector>
#include<queue>
#include<stack>
#include<deque>
#include<map>
#include<unordered_map>
#include<limits>
#include<numeric>
#include<string>
#include<algorithm>
#include<memory>
#include<variant>
#include<optional>
#include<tuple>
#include<format>
#include<cmath>
#include<ctime>
#include<set>
#include<format>

using namespace std;

enum class OrderType
{
    GoodToCancel,
    FillAndKill
};

enum class Side
{
    Buy,
    Sell
};

using Price = std::int32_t;  // using is the new typedef used in before cpp11 features Price is our own datatype now memory 32 bit or 4 byte integer type 
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

struct LevelInfo //used in public apis to get info about state of the orderbook
{

    Price price_;
    Quantity quantity_;
};

using LevelInfos = vector<LevelInfo>; 

class OrderbookLevelInfos
{
    public:
    

        OrderbookLevelInfos(const LevelInfos &bids , const LevelInfos &asks)

           : bids_{ bids }
           , asks_{ asks }
        
        {} 
        
        const LevelInfos& GetBids() const { return bids_ ;}
        const LevelInfos& GetAsks() const { return asks_ ;}

    private:
        LevelInfos bids_;
        LevelInfos asks_;


};

class Order
{
    public:
        

        Order(vector<int> &yy, OrderType orderType , OrderId orderID , Side side , Price price , Quantity quantity) 
            : orderType_{orderType}
            , orderId_{orderId}
            , side_{side}
            , price_{price}
            , quantity_{quantity}
            , initialQuantity_{quantity}
            , remainingQuantity_{quantity}
        
        {}
        
        OrderId GetOrderID() const { return orderId_;}
        Side GetSide() const {return side_;}
        Price GetPrice() const {return price_;}
        OrderType GetOrderType() const {return orderType_;}
        Quantity GetInitialQuantity() const {return initialQuantity_;}
        Quantity GetRemainingQuantity() const {return remainingQuantity_;}
        Quantity GetFilledQuantity() const {return GetInitialQuantity() - GetRemainingQuantity();}

        void fill(Quantity quantity)
        {
            if(quantity> GetRemainingQuantity())
            {
                throw logic_error(std::format(" Order ({}) cannot be filled for more then its remaining quantity", GetOrderID()));
                
                remainingQuantity_ -= quantity
                 
            } 
        }


};
