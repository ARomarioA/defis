// Copyright 2018 The Beam Team / Copyright 2019 The Grimm Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet/wallet_network.h"
#include "core/common.h"

#include "wallet/wallet.h"
#include "wallet/wallet_db.h"
#include "wallet/wallet_network.h"
#include "wallet/secstring.h"
#include "wallet/litecoin/options.h"
#include "wallet/bitcoin/options.h"
#include "wallet/swaps/common.h"
#include "wallet/swaps/swap_transaction.h"
#include "core/ecc_native.h"
#include "core/serialization_adapters.h"
#include "core/treasury.h"
#include "core/block_rw.h"
#include "unittests/util.h"
#include "mnemonic/mnemonic.h"
#include "utility/string_helpers.h"

#ifndef LOG_VERBOSE_ENABLED
    #define LOG_VERBOSE_ENABLED 0
#endif

#include "utility/cli/options.h"
#include "utility/log_rotation.h"
#include "utility/helpers.h"
#include <iomanip>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <iterator>
#include <future>
#include "version.h"

using namespace std;
using namespace grimm;
using namespace grimm::wallet;
using namespace ECC;

namespace grimm
{
    std::ostream& operator<<(std::ostream& os, Coin::Status s)
    {
        stringstream ss;
        ss << "[";
        switch (s)
        {
        case Coin::Available: ss << "Available"; break;
        case Coin::Unavailable: ss << "Unavailable"; break;
        case Coin::Spent: ss << "Spent"; break;
        case Coin::Maturing: ss << "Maturing"; break;
        case Coin::Outgoing: ss << "In progress(outgoing)"; break;
        case Coin::Incoming: ss << "In progress(incoming/change)"; break;
        default:
            assert(false && "Unknown coin status");
        }
        ss << "]";
        string str = ss.str();
        os << str;
        assert(str.length() <= 30);
        return os;
    }

    const char* getTxStatus(const TxDescription& tx)
    {
        static const char* Pending = "pending";
        static const char* WaitingForSender = "waiting for sender";
        static const char* WaitingForReceiver = "waiting for receiver";
        static const char* Sending = "sending";
        static const char* Receiving = "receiving";
        static const char* Cancelled = "cancelled";
        static const char* Sent = "sent";
        static const char* Received = "received";
        static const char* Failed = "failed";
        static const char* Completed = "completed";
        static const char* Expired = "expired";

        switch (tx.m_status)
        {
        case TxStatus::Pending: return Pending;
        case TxStatus::InProgress: return tx.m_sender ? WaitingForReceiver : WaitingForSender;
        case TxStatus::Registering: return tx.m_sender ? Sending : Receiving;
        case TxStatus::Cancelled: return Cancelled;
        case TxStatus::Completed:
        {
            if (tx.m_selfTx)
            {
                return Completed;
            }
            return tx.m_sender ? Sent : Received;
        }
        case TxStatus::Failed: return TxFailureReason::TransactionExpired == tx.m_failureReason ? Expired : Failed;
        default:
            assert(false && "Unknown status");
        }

        return "";
    }

    const char* getSwapTxStatus(const IWalletDB::Ptr& walletDB, const TxDescription& tx)
    {
        static const char* Initial = "initial";
        static const char* Invitation = "invitation";
        static const char* BuildingGrimmLockTX = "building Grimm LockTX";
        static const char* BuildingGrimmRefundTX = "building Grimm RefundTX";
        static const char* BuildingGrimmRedeemTX = "building Grimm RedeemTX";
        static const char* HandlingContractTX = "handling LockTX";
        static const char* SendingRefundTX = "sending RefundTX";
        static const char* SendingRedeemTX = "sending RedeemTX";
        static const char* SendingGrimmLockTX = "sending Grimm LockTX";
        static const char* SendingGrimmRefundTX = "sending Grimm RefundTX";
        static const char* SendingGrimmRedeemTX = "sending Grimm RedeemTX";
        static const char* Completed = "completed";
        static const char* Cancelled = "cancelled";
        static const char* Aborted = "aborted";
        static const char* Failed = "failed";
        static const char* Expired = "expired";

        wallet::AtomicSwapTransaction::State state = wallet::AtomicSwapTransaction::State::CompleteSwap;
        storage::getTxParameter(*walletDB, tx.m_txId, wallet::TxParameterID::State, state);

        switch (state)
        {
        case wallet::AtomicSwapTransaction::State::Initial:
            return Initial;
        case wallet::AtomicSwapTransaction::State::Invitation:
            return Invitation;
        case wallet::AtomicSwapTransaction::State::BuildingGrimmLockTX:
            return BuildingGrimmLockTX;
        case wallet::AtomicSwapTransaction::State::BuildingGrimmRefundTX:
            return BuildingGrimmRefundTX;
        case wallet::AtomicSwapTransaction::State::BuildingGrimmRedeemTX:
            return BuildingGrimmRedeemTX;
        case wallet::AtomicSwapTransaction::State::HandlingContractTX:
            return HandlingContractTX;
        case wallet::AtomicSwapTransaction::State::SendingRefundTX:
            return SendingRefundTX;
        case wallet::AtomicSwapTransaction::State::SendingRedeemTX:
            return SendingRedeemTX;
        case wallet::AtomicSwapTransaction::State::SendingGrimmLockTX:
            return SendingGrimmLockTX;
        case wallet::AtomicSwapTransaction::State::SendingGrimmRefundTX:
            return SendingGrimmRefundTX;
        case wallet::AtomicSwapTransaction::State::SendingGrimmRedeemTX:
            return SendingGrimmRedeemTX;
        case wallet::AtomicSwapTransaction::State::CompleteSwap:
            return Completed;
        case wallet::AtomicSwapTransaction::State::Cancelled:
            return Cancelled;
        case wallet::AtomicSwapTransaction::State::Refunded:
            return Aborted;
        case wallet::AtomicSwapTransaction::State::Failed:
        {
            TxFailureReason reason = TxFailureReason::Unknown;
            storage::getTxParameter(*walletDB, tx.m_txId, wallet::TxParameterID::InternalFailureReason, reason);

            return TxFailureReason::TransactionExpired == reason ? Expired : Failed;
        }
        default:
            assert(false && "Unexpected status");
        }

        return "";
    }

    const char* getAtomicSwapCoinText(AtomicSwapCoin swapCoin)
    {
        switch (swapCoin)
        {
        case AtomicSwapCoin::Bitcoin:
            return "BTC";
        case AtomicSwapCoin::Litecoin:
            return "LTC";
        default:
            assert(false && "Unknow SwapCoin");
        }
        return "";
    }
}
namespace
{
    void ResolveWID(PeerID& res, const std::string& s)
    {
        bool bValid = true;
        ByteBuffer bb = from_hex(s, &bValid);

        if ((bb.size() != res.nBytes) || !bValid)
            throw std::runtime_error("invalid WID");

        memcpy(res.m_pData, &bb.front(), res.nBytes);
    }

    template <typename T>
    bool FLoad(T& x, const std::string& sPath, bool bStrict = true)
    {
        std::FStream f;
        if (!f.Open(sPath.c_str(), true, bStrict))
            return false;

        yas::binary_iarchive<std::FStream, SERIALIZE_OPTIONS> arc(f);
        arc & x;
        return true;
    }

    template <typename T>
    void FSave(const T& x, const std::string& sPath)
    {
        std::FStream f;
        f.Open(sPath.c_str(), false, true);

        yas::binary_oarchive<std::FStream, SERIALIZE_OPTIONS> arc(f);
        arc & x;
    }

    int HandleTreasury(const po::variables_map& vm, Key::IKdf& kdf)
    {
        PeerID wid;
        Scalar::Native sk;
        Treasury::get_ID(kdf, wid, sk);

        char szID[PeerID::nTxtLen + 1];
        wid.Print(szID);

        static const char* szPlans = "treasury_plans.bin";
        static const char* szRequest = "-plan.bin";
        static const char* szResponse = "-response.bin";
        static const char* szData = "treasury_data.bin";

        Treasury tres;
        FLoad(tres, szPlans, false);


        auto nCode = vm[cli::TR_OPCODE].as<uint32_t>();
        switch (nCode)
        {
        default:
            cout << "ID: " << szID << std::endl;
            break;

        case 1:
        {
            // generate plan
            std::string sID = vm[cli::TR_WID].as<std::string>();
            ResolveWID(wid, sID);

            auto perc = vm[cli::TR_PERC].as<double>();

            bool bConsumeRemaining = (perc <= 0.);
            if (bConsumeRemaining)
                perc = vm[cli::TR_PERC_TOTAL].as<double>();

            perc *= 0.01;

            Amount val = static_cast<Amount>(Rules::get().Emission.Value0 * perc); // rounded down

            Treasury::Parameters pars; // default

            uint32_t m = vm[cli::TR_M].as<uint32_t>();
            uint32_t n = vm[cli::TR_N].as<uint32_t>();

            if (m >= n)
                throw std::runtime_error("bad m/n");

            assert(n);
            if (pars.m_Bursts % n)
                throw std::runtime_error("bad n (roundoff)");

            pars.m_Bursts /= n;
            pars.m_Maturity0 = pars.m_MaturityStep * pars.m_Bursts * m;

            Treasury::Entry* pE = tres.CreatePlan(wid, val, pars);

            if (bConsumeRemaining)
            {
                // special case - consume the remaining
                for (size_t iG = 0; iG < pE->m_Request.m_vGroups.size(); iG++)
                {
                    Treasury::Request::Group& g = pE->m_Request.m_vGroups[iG];
                    Treasury::Request::Group::Coin& c = g.m_vCoins[0];

                    AmountBig::Type valInBurst = Zero;

                    for (Treasury::EntryMap::const_iterator it = tres.m_Entries.begin(); tres.m_Entries.end() != it; it++)
                    {
                        if (&it->second == pE)
                            continue;

                        const Treasury::Request& r2 = it->second.m_Request;
                        for (size_t iG2 = 0; iG2 < r2.m_vGroups.size(); iG2++)
                        {
                            const Treasury::Request::Group& g2 = r2.m_vGroups[iG2];
                            if (g2.m_vCoins[0].m_Incubation != c.m_Incubation)
                                continue;

                            for (size_t i = 0; i < g2.m_vCoins.size(); i++)
                                valInBurst += uintBigFrom(g2.m_vCoins[i].m_Value);
                        }
                    }

                    Amount vL = AmountBig::get_Lo(valInBurst);
                    if (AmountBig::get_Hi(valInBurst) || (vL >= c.m_Value))
                        throw std::runtime_error("Nothing remains");

                    cout << "Maturity=" << c.m_Incubation << ", Consumed = " << vL << " / " << c.m_Value << std::endl;
                    c.m_Value -= vL;
                }

            }

            FSave(pE->m_Request, sID + szRequest);
            FSave(tres, szPlans);
        }
        break;

        case 2:
        {
            // generate response
            Treasury::Request treq;
            FLoad(treq, std::string(szID) + szRequest);

            Treasury::Response tresp;
            uint64_t nIndex = 1;
            tresp.Create(treq, kdf, nIndex);

            FSave(tresp, std::string(szID) + szResponse);
        }
        break;

        case 3:
        {
            // verify & import reponse
            std::string sID = vm[cli::TR_WID].as<std::string>();
            ResolveWID(wid, sID);

            Treasury::EntryMap::iterator it = tres.m_Entries.find(wid);
            if (tres.m_Entries.end() == it)
                throw std::runtime_error("plan not found");

            Treasury::Entry& e = it->second;
            e.m_pResponse.reset(new Treasury::Response);
            FLoad(*e.m_pResponse, sID + szResponse);

            if (!e.m_pResponse->IsValid(e.m_Request))
                throw std::runtime_error("invalid response");

            FSave(tres, szPlans);
        }
        break;

        case 4:
        {
            // Finally generate treasury
            Treasury::Data data;
            data.m_sCustomMsg = vm[cli::TR_COMMENT].as<std::string>();
            tres.Build(data);

            FSave(data, szData);

            Serializer ser;
            ser & data;

            ByteBuffer bb;
            ser.swap_buf(bb);

            Hash::Value hv;
            Hash::Processor() << Blob(bb) >> hv;

            char szHash[Hash::Value::nTxtLen + 1];
            hv.Print(szHash);

            cout << "Treasury data hash: " << szHash << std::endl;

        }
        break;

        case 5:
        {
            // recover and print
            Treasury::Data data;
            FLoad(data, szData);

            std::vector<Treasury::Data::Coin> vCoins;
            data.Recover(kdf, vCoins);

            cout << "Recovered coins: " << vCoins.size() << std::endl;

            for (size_t i = 0; i < vCoins.size(); i++)
            {
                const Treasury::Data::Coin& coin = vCoins[i];
                cout << "\t" << coin.m_Kidv << ", Height=" << coin.m_Incubation << std::endl;

            }
        }
        break;

        case 6:
        {
            // bursts
            Treasury::Data data;
            FLoad(data, szData);

            auto vBursts = data.get_Bursts();

            cout << "Total bursts: " << vBursts.size() << std::endl;

            for (size_t i = 0; i < vBursts.size(); i++)
            {
                const Treasury::Data::Burst& b = vBursts[i];
                cout << "\t" << "Height=" << b.m_Height << ", Value=" << b.m_Value << std::endl;
            }
        }
        break;
        }

        return 0;
    }

    void printHelp(const po::options_description& options)
    {
        cout << options << std::endl;
    }

    int ChangeAddressExpiration(const po::variables_map& vm, const IWalletDB::Ptr& walletDB)
    {
        string address = vm[cli::WALLET_ADDR].as<string>();
        string newTime = vm[cli::EXPIRATION_TIME].as<string>();
        WalletID walletID(Zero);
        bool allAddresses = address == "*";

        if (!allAddresses)
        {
            walletID.FromHex(address);
        }
        uint64_t newDuration_s = 0;
        if (newTime == "24h")
        {
            newDuration_s = 24 * 3600; //seconds
        }
        else if (newTime == "never")
        {
            newDuration_s = 0;
        }
        else
        {
            LOG_ERROR() << "Invalid address expiration time \"" << newTime << "\".";
            return -1;
        }

        if (storage::changeAddressExpiration(*walletDB, walletID, newDuration_s))
        {
            if (allAddresses)
            {
                LOG_INFO() << "Expiration for all addresses  was changed to \"" << newTime << "\".";
            }
            else
            {
                LOG_INFO() << "Expiration for address " << to_string(walletID) << " was changed to \"" << newTime << "\".";
            }
            return 0;
        }
        return -1;
    }

    WalletAddress CreateNewAddress(const IWalletDB::Ptr& walletDB, const std::string& comment, bool isNever = false)
    {
        WalletAddress address = storage::createAddress(*walletDB);

        if (isNever)
        {
            address.m_duration = 0;
        }

        address.m_label = comment;
        walletDB->saveAddress(address);

        LOG_INFO() << "New address generated:\n\n" << std::to_string(address.m_walletID) << "\n";
        if (!comment.empty()) {
            LOG_INFO() << "comment = " << comment;
        }
        return address;
    }

    WordList GeneratePhrase()
    {
        auto phrase = createMnemonic(getEntropy(), language::en);
        assert(phrase.size() == 12);
        cout << "======\nGenerated seed phrase: \n\n\t";
        for (const auto& word : phrase)
        {
            cout << word << ';';
        }
        cout << "\n\n\tIMPORTANT\n\n\tYour seed phrase is the access key to all the cryptocurrencies in your wallet.\n\tPrint or write down the phrase to keep it in a safe or in a locked vault.\n\tWithout the phrase you will not be able to recover your money.\n======" << endl;
        return phrase;
    }

    bool ReadWalletSeed(NoLeak<uintBig>& walletSeed, const po::variables_map& vm, bool generateNew)
    {
        SecString seed;
        WordList phrase;
        if (generateNew)
        {
            LOG_INFO() << "Generating seed phrase...";
            phrase = GeneratePhrase();
        }
        else if (vm.count(cli::SEED_PHRASE))
        {
            auto tempPhrase = vm[cli::SEED_PHRASE].as<string>();
            boost::algorithm::trim_if(tempPhrase, [](char ch) { return ch == ';'; });
            phrase = string_helpers::split(tempPhrase, ';');
            assert(phrase.size() == WORD_COUNT);
            if (!isValidMnemonic(phrase, language::en))
            {
                LOG_ERROR() << "Invalid seed phrase provided: " << tempPhrase;
                return false;
            }
        }
        else
        {
            LOG_ERROR() << "Seed phrase has not been provided.";
            return false;
        }

        auto buf = decodeMnemonic(phrase);
        seed.assign(buf.data(), buf.size());

        walletSeed.V = seed.hash().V;
        return true;
    }

    int ShowAddressList(const IWalletDB::Ptr& walletDB)
    {
        auto addresses = walletDB->getAddresses(true);
        array<uint8_t, 5> columnWidths{ { 20, 70, 8, 20, 21 } };

        // Comment | Address | Active | Expiration date | Created |
        cout << "Addresses\n\n"
            << "  " << std::left
            << setw(columnWidths[0]) << "comment" << "|"
            << setw(columnWidths[1]) << "address" << "|"
            << setw(columnWidths[2]) << "active" << "|"
            << setw(columnWidths[3]) << "expiration date" << "|"
            << setw(columnWidths[4]) << "created" << endl;

        for (const auto& address : addresses)
        {
            auto comment = address.m_label;

            if (comment.length() > columnWidths[0])
            {
                comment = comment.substr(0, columnWidths[0] - 3) + "...";
            }

            auto expirationDateText = (address.m_duration == 0) ? "never" : format_timestamp("%Y.%m.%d %H:%M:%S", address.getExpirationTime() * 1000, false);

            cout << "  " << std::left << std::boolalpha
                << setw(columnWidths[0]) << comment << " "
                << setw(columnWidths[1]) << std::to_string(address.m_walletID) << " "
                << setw(columnWidths[2]) << !address.isExpired() << " "
                << setw(columnWidths[3]) << expirationDateText << " "
                << setw(columnWidths[4]) << format_timestamp("%Y.%m.%d %H:%M:%S", address.getCreateTime() * 1000, false) << "\n";
        }

        return 0;
    }

    bool FromHex(AssetID& assetID, const std::string& s)
    {
        LOG_INFO() << "Parsing string " << s;
        bool bValid = true;

        ByteBuffer bb = from_hex(s, &bValid);
        if (!bValid)
        {
            LOG_ERROR() << "Invalid AssetID (hex string)";
            return false;
        }
        if (bb.size() != sizeof(assetID))
        {
            LOG_ERROR() << "Invalid AssetID (size string)";
            return false;
        }

        typedef uintBig_t<sizeof(assetID)> BigSelf;
        static_assert(sizeof(BigSelf) == sizeof(assetID), "");

        *reinterpret_cast<BigSelf*>(&assetID) = Blob(bb);

        LOG_INFO() << "Parsing string ok. AssetID: " << assetID;
        return true;
    }


    int ShowWalletInfo(const IWalletDB::Ptr& walletDB, const po::variables_map& vm)
    {
        Block::SystemState::ID stateID = {};
        walletDB->getSystemStateID(stateID);
        AssetID assetID = Zero;

        if (vm.count(cli::ASSET_ID))
        {
            if (!FromHex(assetID, vm[cli::ASSET_ID].as<string>()))
            {
                cout << "AssetID Parsing failed...\n";
                return -1;
            }
        }

        storage::Totals totals(*walletDB, assetID);

        if (vm.count(cli::ASSET_ID))
        {
          cout << "____Asset Wallet____\n\n"
              << "AssetID .................." << assetID << "\n\n"
              << "Current height............" << stateID.m_Height << '\n'
              << "Current state ID.........." << stateID.m_Hash << "\n\n"
              << "Available................." << PrintableAmount(totals.Avail) << " assets" << '\n'
              << "Maturing.................." << PrintableAmount(totals.Maturing) << '\n'
              << "In progress..............." << PrintableAmount(totals.Incoming) << '\n'
              << "Unavailable..............." << PrintableAmount(totals.Unavail) << '\n'
              << "Available coinbase ......." << PrintableAmount(totals.AvailCoinbase) << '\n'
              << "Total coinbase............" << PrintableAmount(totals.Coinbase) << '\n'
              << "Avaliable fee............." << PrintableAmount(totals.AvailFee) << '\n'
              << "Total fee................." << PrintableAmount(totals.Fee) << '\n'
              << "Total unspent............." << PrintableAmount(totals.Unspent) << " assets" << "\n\n";
        } else {

        cout << "____Wallet summary____\n\n"
            << "Current height............" << stateID.m_Height << '\n'
            << "Current state ID.........." << stateID.m_Hash << "\n\n"
            << "Available................." << PrintableAmount(totals.Avail) << '\n'
            << "Maturing.................." << PrintableAmount(totals.Maturing) << '\n'
            << "In progress..............." << PrintableAmount(totals.Incoming) << '\n'
            << "Unavailable..............." << PrintableAmount(totals.Unavail) << '\n'
            << "Available coinbase ......." << PrintableAmount(totals.AvailCoinbase) << '\n'
            << "Total coinbase............" << PrintableAmount(totals.Coinbase) << '\n'
            << "Avaliable fee............." << PrintableAmount(totals.AvailFee) << '\n'
            << "Total fee................." << PrintableAmount(totals.Fee) << '\n'
            << "Total unspent............." << PrintableAmount(totals.Unspent) << "\n\n";
        }
        if (vm.count(cli::TX_HISTORY))
        {
            auto txHistory = walletDB->getTxHistory();
            if (txHistory.empty())
            {
                cout << "No transactions\n";
                return 0;
            }

            const array<uint8_t, 6> columnWidths{ { 20, 17, 26, 21, 33, 65} };

            if (vm.count(cli::ASSET_ID))
            {
            cout << "TRANSACTIONS\n\n  |"
                << left << setw(columnWidths[0]) << " datetime" << " |"
                << left << setw(columnWidths[1]) << " direction" << " |"
                << right << setw(columnWidths[2]) << " assets amount" << " |"
                << left << setw(columnWidths[3]) << " status" << " |"
                << setw(columnWidths[4]) << " ID" << " |"
                << setw(columnWidths[5]) << " kernel ID" << " |" << endl;
            } else {
              cout << "TRANSACTIONS\n\n  |"
                  << left << setw(columnWidths[0]) << " datetime" << " |"
                  << left << setw(columnWidths[1]) << " direction" << " |"
                  << right << setw(columnWidths[2]) << " amount " << vm[cli::CAC_SYMBOL].as<string>() << " |"
                  << left << setw(columnWidths[3]) << " status" << " |"
                  << setw(columnWidths[4]) << " ID" << " |"
                  << setw(columnWidths[5]) << " kernel ID" << " |" << endl;
            }

            for (auto& tx : txHistory)
            {
                cout << "   "
                    << " " << left << setw(columnWidths[0]) << format_timestamp("%Y.%m.%d %H:%M:%S", tx.m_createTime * 1000, false) << " "
                    << " " << left << setw(columnWidths[1]) << (tx.m_selfTx ? "self transaction" : (tx.m_sender ? "outgoing" : "incoming"))
                    << " " << right << setw(columnWidths[2]) << PrintableAmount(tx.m_amount, true) << "  "
                    << " " << left << setw(columnWidths[3]+1) << getTxStatus(tx)
                    << " " << setw(columnWidths[4]+1) << to_hex(tx.m_txId.data(), tx.m_txId.size())
                    << " " << setw(columnWidths[5]+1) << to_string(tx.m_kernelID) << '\n';
            }
            return 0;
        }

        if (vm.count(cli::SWAP_TX_HISTORY))
        {
            auto txHistory = walletDB->getTxHistory(wallet::TxType::AtomicSwap);
            if (txHistory.empty())
            {
                cout << "No swap transactions\n";
                return 0;
            }

            const array<uint8_t, 6> columnWidths{ { 20, 26, 18, 15, 23, 33} };

            cout << "SWAP TRANSACTIONS\n\n  |"
                << left << setw(columnWidths[0]) << " datetime" << " |"
                << right << setw(columnWidths[1]) << " amount, XGM" << " |"
                << right << setw(columnWidths[2]) << " swap amount" << " |"
                << left << setw(columnWidths[3]) << " swap type" << " |"
                << left << setw(columnWidths[4]) << " status" << " |"
                << setw(columnWidths[5]) << " ID" << " |" << endl;

            for (auto& tx : txHistory)
            {
                Amount swapAmount = 0;
                storage::getTxParameter(*walletDB, tx.m_txId, wallet::kDefaultSubTxID, wallet::TxParameterID::AtomicSwapAmount, swapAmount);
                bool isGrimmSide = false;
                storage::getTxParameter(*walletDB, tx.m_txId, wallet::kDefaultSubTxID, wallet::TxParameterID::AtomicSwapIsGrimmSide, isGrimmSide);

                AtomicSwapCoin swapCoin = AtomicSwapCoin::Unknown;
                storage::getTxParameter(*walletDB, tx.m_txId, wallet::kDefaultSubTxID, wallet::TxParameterID::AtomicSwapCoin, swapCoin);

                stringstream ss;
                ss << (isGrimmSide ? "Grimm" : getAtomicSwapCoinText(swapCoin)) << " <--> " << (!isGrimmSide ? "Grimm" : getAtomicSwapCoinText(swapCoin));

                cout << "   "
                    << " " << left << setw(columnWidths[0]) << format_timestamp("%Y.%m.%d %H:%M:%S", tx.m_createTime * 1000, false)
                    << " " << right << setw(columnWidths[1]) << PrintableAmount(tx.m_amount, true) << " "
                    << " " << right << setw(columnWidths[2]) << swapAmount << " "
                    << " " << right << setw(columnWidths[3]) << ss.str() << "  "
                    << " " << left << setw(columnWidths[4]) << getSwapTxStatus(walletDB, tx)
                    << " " << setw(columnWidths[5] + 1) << to_hex(tx.m_txId.data(), tx.m_txId.size()) << '\n';
            }
            return 0;
        }

        const array<uint8_t, 6> columnWidths{ { 49, 14, 14, 18, 30, 8} };
        if (vm.count(cli::ASSET_ID))
        {
        cout << "AssetID .................." << assetID << "\n";
        cout << "  |"
            << left << setw(columnWidths[0]) << " ID" << " |"
            << right << setw(columnWidths[1]) << " assets" << " |"
            << setw(columnWidths[2]) << " centum" << " |"
            << left << setw(columnWidths[3]) << " maturity" << " |"
            << setw(columnWidths[4]) << " status" << " |"
            << setw(columnWidths[5]) << " type" << endl;
        } else {
          cout << "  |"
              << left << setw(columnWidths[0]) << " ID" << " |"
              << right << setw(columnWidths[1]) << " " <<  vm[cli::CAC_SYMBOL].as<string>() << " |"
              << setw(columnWidths[2]) << " centum" << " |"
              << left << setw(columnWidths[3]) << " maturity" << " |"
              << setw(columnWidths[4]) << " status" << " |"
              << setw(columnWidths[5]) << " type" << endl;
        }


        walletDB->visit([&columnWidths](const Coin& c)->bool
        {
            cout << "   "
                << " " << left << setw(columnWidths[0]) << c.toStringID()
                << " " << right << setw(columnWidths[1]) << c.m_ID.m_Value / Rules::Coin << " "
                << " " << right << setw(columnWidths[2]) << c.m_ID.m_Value % Rules::Coin << "  "
                << " " << left << setw(columnWidths[3]+1) << (c.IsMaturityValid() ? std::to_string(static_cast<int64_t>(c.m_maturity)) : "-")
                << " " << setw(columnWidths[4]+1) << c.m_status
                << " " << setw(columnWidths[5]+1) << c.m_ID.m_Type << endl;
            return true;
        }, assetID);
        return 0;
    }

    int TxDetails(const IWalletDB::Ptr& walletDB, const po::variables_map& vm)
    {
        auto txIdStr = vm[cli::TX_ID].as<string>();
        if (txIdStr.empty()) {
            LOG_ERROR() << "Failed, --tx_id param required";
            return -1;
        }
        auto txIdVec = from_hex(txIdStr);
        TxID txId;
        if (txIdVec.size() >= 16)
            std::copy_n(txIdVec.begin(), 16, txId.begin());

        auto tx = walletDB->getTx(txId);
        if (!tx)
        {
            LOG_ERROR() << "Failed, transaction with id: "
                        << txIdStr
                        << " does not exist.";
            return -1;
        }

        LOG_INFO() << "Transaction details:\n"
                   << storage::TxDetailsInfo(walletDB, txId)
                   << "Status: "
                   << getTxStatus(*tx)
                   << (tx->m_status == TxStatus::Failed ? "\nReason: "+ GetFailureMessage(tx->m_failureReason) : "");

        return 0;
    }

    int ExportPaymentProof(const IWalletDB::Ptr& walletDB, const po::variables_map& vm)
    {
        auto txIdVec = from_hex(vm[cli::TX_ID].as<string>());
        TxID txId;
        if (txIdVec.size() >= 16)
            std::copy_n(txIdVec.begin(), 16, txId.begin());

        auto tx = walletDB->getTx(txId);
        if (!tx)
        {
            LOG_ERROR() << "Failed to export payment proof, transaction does not exist.";
            return -1;
        }
        if (!tx->m_sender || tx->m_selfTx)
        {
            LOG_ERROR() << "Cannot export payment proof for receiver or self transaction.";
            return -1;
        }
        if (tx->m_status != TxStatus::Completed)
        {
            LOG_ERROR() << "Failed to export payment proof. Transaction is not completed.";
            return -1;
        }

        auto res = storage::ExportPaymentProof(*walletDB, txId);
        if (!res.empty())
        {
            std::string sTxt;
            sTxt.resize(res.size() * 2);

            grimm::to_hex(&sTxt.front(), res.data(), res.size());
            LOG_INFO() << "Exported form: " << sTxt;
        }

        return 0;
    }

    int VerifyPaymentProof(const po::variables_map& vm)
    {
        const auto& pprofData = vm[cli::PAYMENT_PROOF_DATA];
        if (pprofData.empty())
        {
            throw std::runtime_error("No payment proof provided: --payment_proof parameter is missing");
        }
        ByteBuffer buf = from_hex(pprofData.as<string>());

        if (!storage::VerifyPaymentProof(buf))
            throw std::runtime_error("Payment proof is invalid");

        return 0;
    }

    int ExportMinerKey(const po::variables_map& vm, const IWalletDB::Ptr& walletDB, const grimm::SecString& pass)
    {
        uint32_t subKey = vm[cli::KEY_SUBKEY].as<Nonnegative<uint32_t>>().value;
        if (subKey < 1)
        {
            cout << "Please, specify Subkey number --subkey=N (N > 0)" << endl;
            return -1;
        }
        Key::IKdf::Ptr pKey = walletDB->get_ChildKdf(subKey);
        const ECC::HKdf& kdf = static_cast<ECC::HKdf&>(*pKey);

        KeyString ks;
        ks.SetPassword(Blob(pass.data(), static_cast<uint32_t>(pass.size())));
        ks.m_sMeta = std::to_string(subKey);

        ks.Export(kdf);
        cout << "Secret Subkey " << subKey << ": " << ks.m_sRes << std::endl;

        return 0;
    }

    int ExportOwnerKey(const IWalletDB::Ptr& walletDB, const grimm::SecString& pass)
    {
        Key::IKdf::Ptr pKey = walletDB->get_ChildKdf(0);
        const ECC::HKdf& kdf = static_cast<ECC::HKdf&>(*pKey);

        KeyString ks;
        ks.SetPassword(Blob(pass.data(), static_cast<uint32_t>(pass.size())));
        ks.m_sMeta = std::to_string(0);

        ECC::HKdfPub pkdf;
        pkdf.GenerateFrom(kdf);

        ks.Export(pkdf);
        cout << "Owner Viewer key: " << ks.m_sRes << std::endl;

        return 0;
    }

    bool LoadDataToImport(const std::string& path, ByteBuffer& data)
    {
        FStream f;
        if (f.Open(path.c_str(), true))
        {
            size_t size = static_cast<size_t>(f.get_Remaining());
            if (size > 0)
            {
                data.resize(size);
                return f.read(data.data(), data.size()) == size;
            }
        }
        return false;
    }

    bool SaveExportedData(const ByteBuffer& data, const std::string& path)
    {
        FStream f;
        if (f.Open(path.c_str(), false))
        {
            return f.write(data.data(), data.size()) == data.size();
        }
        LOG_ERROR() << "Failed to save exported data";
        return false;
    }

    int ExportAddresses(const po::variables_map& vm, const IWalletDB::Ptr& walletDB)
    {
        auto s = storage::ExportAddressesToJson(*walletDB);
        return SaveExportedData(ByteBuffer(s.begin(), s.end()), vm[cli::IMPORT_EXPORT_PATH].as<string>()) ? 0 : -1;
    }

    int ImportAddresses(const po::variables_map& vm, const IWalletDB::Ptr& walletDB)
    {
        ByteBuffer buffer;
        if (!LoadDataToImport(vm[cli::IMPORT_EXPORT_PATH].as<string>(), buffer))
        {
            return -1;
        }
        const char* p = (char*)(&buffer[0]);
        return storage::ImportAddressesFromJson(*walletDB, p, buffer.size()) ? 0 : -1;
    }

    CoinIDList GetPreselectedCoinIDs(const po::variables_map& vm)
    {
        CoinIDList coinIDs;
        if (vm.count(cli::UTXO))
        {
            auto tempCoins = vm[cli::UTXO].as<vector<string>>();
            for (const auto& s : tempCoins)
            {
                auto csv = string_helpers::split(s, ',');
                for (const auto& v : csv)
                {
                    auto coinID = Coin::FromString(v);
                    if (coinID)
                    {
                        coinIDs.push_back(*coinID);
                    }
                }
            }
        }
        return coinIDs;
    }

    bool LoadAssetParamsForTX(const po::variables_map& vm, AssetCommand& assetCommand, uint64_t& idx, AssetID& assetID)
    {
        if (vm.count(cli::ASSET_OPCODE) == 0)
        {
            return false;
        }
        if (vm.count(cli::ASSET_KID) == 0 && vm.count(cli::ASSET_ID) == 0)
        {
            LOG_ERROR() << "--- asset_id or asset_kid required";
            return false;
        }

        switch (vm[cli::ASSET_OPCODE].as<uint32_t>())
        {
            case 1: assetCommand = AssetCommand::Issue; break;
            case 2: assetCommand = AssetCommand::Transfer; break;
            case 3: assetCommand = AssetCommand::Burn; break;
            default: LOG_ERROR() << "confidential asset operation: 1=emission, 2=send, 3=burn"; return false;
        }

        if (assetCommand == AssetCommand::Transfer)
        {
            if (!FromHex(assetID, vm[cli::ASSET_ID].as<string>()))
            {
                LOG_ERROR() << "Invalid asset_id";
                return false;
            }
        }
        else
        {
            idx = vm[cli::ASSET_KID].as<uint64_t>();
        }

        return true;
    }


    bool LoadBaseParamsForTX(const po::variables_map& vm, Amount& amount, Amount& fee, WalletID& receiverWalletID, bool checkFee)
    {
        if (vm.count(cli::RECEIVER_ADDR) == 0)
        {
            LOG_ERROR() << "receiver's address is missing";
            return false;
        }
        if (vm.count(cli::AMOUNT) == 0)
        {
            LOG_ERROR() << "amount is missing";
            return false;
        }

        receiverWalletID.FromHex(vm[cli::RECEIVER_ADDR].as<string>());

        auto signedAmount = vm[cli::AMOUNT].as<Positive<double>>().value;
        if (signedAmount < 0)
        {
            LOG_ERROR() << "Unable to send negative amount of coins";
            return false;
        }

        signedAmount *= Rules::Coin; // convert grimms to coins

        amount = static_cast<ECC::Amount>(std::round(signedAmount));
        if (amount == 0)
        {
            LOG_ERROR() << "Unable to send zero coins";
            return false;
        }

        fee = vm[cli::FEE].as<Nonnegative<Amount>>().value;
        if (checkFee && fee < cli::kMinimumFee)
        {
            LOG_ERROR() << "Failed to initiate the send operation. The minimum fee is 100 centum.";
            return false;
        }

        return true;
    }
}

io::Reactor::Ptr reactor;

static const unsigned LOG_ROTATION_PERIOD_SEC = 3*60*60; // 3 hours

int main_impl(int argc, char* argv[])
{
    grimm::Crash::InstallHandler(NULL);

    try
    {
        auto [options, visibleOptions] = createOptionsDescription(GENERAL_OPTIONS | WALLET_OPTIONS);

        po::variables_map vm;
        try
        {
            vm = getOptions(argc, argv, "defis-wallet.cfg", options, true);
        }
        catch (const po::invalid_option_value& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const NonnegativeOptionException& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const PositiveOptionException& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const po::error& e)
        {
            cout << e.what() << std::endl;
            printHelp(visibleOptions);

            return 0;
        }

        if (vm.count(cli::HELP))
        {
            printHelp(visibleOptions);

            return 0;
        }

        if (vm.count(cli::VERSION))
        {
            cout << PROJECT_VERSION << endl;
            return 0;
        }

        if (vm.count(cli::GIT_COMMIT_HASH))
        {
            cout << GIT_COMMIT_HASH << endl;
            return 0;
        }

        int logLevel = getLogLevel(cli::LOG_LEVEL, vm, LOG_LEVEL_DEBUG);
        int fileLogLevel = getLogLevel(cli::FILE_LOG_LEVEL, vm, LOG_LEVEL_DEBUG);

#define LOG_FILES_DIR "logs"
#define LOG_FILES_PREFIX "wallet_"

        const auto path = boost::filesystem::system_complete(LOG_FILES_DIR);
        auto logger = grimm::Logger::create(logLevel, logLevel, fileLogLevel, LOG_FILES_PREFIX, path.string());

        try
        {
            po::notify(vm);

            unsigned logCleanupPeriod = vm[cli::LOG_CLEANUP_DAYS].as<uint32_t>() * 24 * 3600;

            clean_old_logfiles(LOG_FILES_DIR, LOG_FILES_PREFIX, logCleanupPeriod);

            Rules::get().UpdateChecksum();

            {
                reactor = io::Reactor::create();
                io::Reactor::Scope scope(*reactor);

                io::Reactor::GracefulIntHandler gih(*reactor);

                LogRotation logRotation(*reactor, LOG_ROTATION_PERIOD_SEC, logCleanupPeriod);

                {
                    if (vm.count(cli::COMMAND) == 0)
                    {
                        LOG_ERROR() << "command parameter not specified.";
                        printHelp(visibleOptions);
                        return 0;
                    }

                    auto command = vm[cli::COMMAND].as<string>();

                    {
                        const string commands[] =
                        {
                            cli::INIT,
                            cli::RESTORE,
                            cli::SEND,
                            //cli::ASSET_EMIT,
                            //cli::ASSET_SEND,
                            //cli::ASSET_BURN,
                            cli::LISTEN,
                            cli::TREASURY,
                            cli::INFO,
                            cli::EXPORT_MINER_KEY,
                            cli::EXPORT_OWNER_KEY,
                            cli::NEW_ADDRESS,
                            cli::CANCEL_TX,
                            cli::DELETE_TX,
                            cli::CHANGE_ADDRESS_EXPIRATION,
                            cli::TX_DETAILS,
                            cli::PAYMENT_PROOF_EXPORT,
                            cli::PAYMENT_PROOF_VERIFY,
                            cli::GENERATE_PHRASE,
                            cli::WALLET_ADDRESS_LIST,
                            cli::WALLET_RESCAN,
                            cli::IMPORT_ADDRESSES,
                            cli::EXPORT_ADDRESSES,
                            cli::SWAP_INIT,
                            cli::SWAP_LISTEN
                        };

                        if (find(begin(commands), end(commands), command) == end(commands))
                        {
                            LOG_ERROR() << "unknown command: \'" << command << "\'";
                            return -1;
                        }
                    }

                    if (command == cli::GENERATE_PHRASE)
                    {
                        GeneratePhrase();
                        return 0;
                    }
                    if (Rules::get().isAssetchain) {
                             LOG_INFO() << "Confidential Assetchain Symbol: " << vm[cli::CAC_SYMBOL].as<string>();
                             LOG_INFO() << vm[cli::CAC_SYMBOL].as<string>() << " Wallet " << PROJECT_VERSION << " (" << BRANCH_NAME << ")";



                       } else {
                    LOG_INFO() << "Grimm Wallet " << PROJECT_VERSION << " (" << BRANCH_NAME << ")";
                    }
                    LOG_INFO() << "Rules signature: " << Rules::get().get_SignatureStr();

                    bool coldWallet = vm.count(cli::COLD_WALLET) > 0;

                    if (coldWallet && command == cli::RESTORE)
                    {
                        LOG_ERROR() << "You can't restore cold wallet.";
                        return -1;
                    }

                    assert(vm.count(cli::WALLET_STORAGE) > 0);
                    auto walletPath = vm[cli::WALLET_STORAGE].as<string>();

                    if (!WalletDB::isInitialized(walletPath) && (command != cli::INIT && command != cli::RESTORE))
                    {
                        LOG_ERROR() << "Please initialize your wallet first... \nExample: defis-wallet --command=init";
                        return -1;
                    }
                    else if (WalletDB::isInitialized(walletPath) && (command == cli::INIT || command == cli::RESTORE))
                    {
                      bool isDirectory;
                        #ifdef WIN32
                                isDirectory = boost::filesystem::is_directory(Utf8toUtf16(walletPath.c_str()));
                        #else
                                isDirectory = boost::filesystem::is_directory(walletPath);
                        #endif

                        if (isDirectory)
                        {
                            walletPath.append("/wallet.db");
                        }
                        else
                        {
                            LOG_ERROR() << "Your wallet is already initialized.";
                            return -1;
                        }
                    }

                    LOG_INFO() << "starting a wallet...";

                    SecString pass;
                    if (!grimm::read_wallet_pass(pass, vm))
                    {
                        LOG_ERROR() << "Please, provide password for the wallet.";
                        return -1;
                    }

                    if ((command == cli::INIT || command == cli::RESTORE) && vm.count(cli::PASS) == 0)
                    {
                        if (!grimm::confirm_wallet_pass(pass))
                        {
                            LOG_ERROR() << "Passwords do not match";
                            return -1;
                        }
                    }

                    if (command == cli::INIT || command == cli::RESTORE)
                    {
                        NoLeak<uintBig> walletSeed;
                        walletSeed.V = Zero;
                        if (!ReadWalletSeed(walletSeed, vm, command == cli::INIT))
                        {
                            LOG_ERROR() << "Please, provide a valid seed phrase for the wallet.";
                            return -1;
                        }
                        auto walletDB = WalletDB::init(walletPath, pass, walletSeed, reactor, coldWallet);
                        if (walletDB)
                        {
                            LOG_INFO() << "wallet successfully created...";

                            // generate default address
                            CreateNewAddress(walletDB, "default");

                            return 0;
                        }
                        else
                        {
                            LOG_ERROR() << "something went wrong, wallet not created...";
                            return -1;
                        }
                    }

                    auto walletDB = WalletDB::open(walletPath, pass, reactor);
                    if (!walletDB)
                    {
                        LOG_ERROR() << "Please check your password. If password is lost, restore wallet.db from latest backup or delete it and restore from seed phrase.";
                        return -1;
                    }

                    const auto& currHeight = walletDB->getCurrentHeight();
                    const auto& fork1Height = Rules::get().pForks[1].m_Height;
                    const bool isFork1 = currHeight >= fork1Height;

                    if (command == cli::CHANGE_ADDRESS_EXPIRATION)
                    {
                        return ChangeAddressExpiration(vm, walletDB);
                    }

                    if (command == cli::EXPORT_MINER_KEY)
                    {
                        return ExportMinerKey(vm, walletDB, pass);
                    }

                    if (command == cli::EXPORT_OWNER_KEY)
                    {
                        return ExportOwnerKey(walletDB, pass);
                    }

                    if (command == cli::EXPORT_ADDRESSES)
                    {
                        return ExportAddresses(vm, walletDB);
                    }

                    if (command == cli::IMPORT_ADDRESSES)
                    {
                        return ImportAddresses(vm, walletDB);
                    }

                    {
                        const auto& var = vm[cli::PAYMENT_PROOF_REQUIRED];
                        if (!var.empty())
                        {
                            bool b = var.as<bool>();
                            uint8_t n = b ? 1 : 0;
                            storage::setVar(*walletDB, storage::g_szPaymentProofRequired, n);

                            cout << "Parameter set: Payment proof required: " << static_cast<uint32_t>(n) << std::endl;
                            return 0;
                        }
                    }

                    if (command == cli::NEW_ADDRESS)
                    {
                        auto comment = vm[cli::NEW_ADDRESS_COMMENT].as<string>();
                        CreateNewAddress(walletDB, comment, vm[cli::EXPIRATION_TIME].as<string>() == "never");

                        if (!vm.count(cli::LISTEN))
                        {
                            return 0;
                        }
                    }

                    LOG_INFO() << "wallet sucessfully opened...";

                    if (command == cli::TREASURY)
                    {
                        return HandleTreasury(vm, *walletDB->get_MasterKdf());
                    }

                    BitcoinOptions btcOptions;
                    if (vm.count(cli::BTC_NODE_ADDR) > 0 || vm.count(cli::BTC_USER_NAME) > 0 || vm.count(cli::BTC_PASS) > 0)
                    {
                        string btcNodeUri = vm[cli::BTC_NODE_ADDR].as<string>();
                        if (!btcOptions.m_address.resolve(btcNodeUri.c_str()))
                        {
                            LOG_ERROR() << "unable to resolve bitcoin node address: " << btcNodeUri;
                            return -1;
                        }

                        if (vm.count(cli::BTC_USER_NAME) == 0)
                        {
                            LOG_ERROR() << "user name of bitcoin node should be specified";
                            return -1;
                        }

                        btcOptions.m_userName = vm[cli::BTC_USER_NAME].as<string>();

                        // TODO roman.strilets: use SecString instead of std::string
                        if (vm.count(cli::BTC_PASS) == 0)
                        {
                            LOG_ERROR() << "Please, provide password for the bitcoin node.";
                            return -1;
                        }

                        btcOptions.m_pass = vm[cli::BTC_PASS].as<string>();
                    }

                    LitecoinOptions ltcOptions;
                    if (vm.count(cli::LTC_NODE_ADDR) > 0 || vm.count(cli::LTC_USER_NAME) > 0 || vm.count(cli::LTC_PASS) > 0)
                    {
                        string ltcNodeUri = vm[cli::LTC_NODE_ADDR].as<string>();
                        if (!ltcOptions.m_address.resolve(ltcNodeUri.c_str()))
                        {
                            LOG_ERROR() << "unable to resolve litecoin node address: " << ltcNodeUri;
                            return -1;
                        }

                        if (vm.count(cli::LTC_USER_NAME) == 0)
                        {
                            LOG_ERROR() << "user name of litecoin node should be specified";
                            return -1;
                        }

                        ltcOptions.m_userName = vm[cli::LTC_USER_NAME].as<string>();

                        // TODO roman.strilets: use SecString instead of std::string
                        if (vm.count(cli::LTC_PASS) == 0)
                        {
                            LOG_ERROR() << "Please, provide password for the litecoin node.";
                            return -1;
                        }

                        ltcOptions.m_pass = vm[cli::LTC_PASS].as<string>();
                    }

                    if (command == cli::INFO)
                    {
                        return ShowWalletInfo(walletDB, vm);
                    }

                    if (command == cli::TX_DETAILS)
                    {
                        return TxDetails(walletDB, vm);
                    }

                    if (command == cli::PAYMENT_PROOF_EXPORT)
                    {
                        return ExportPaymentProof(walletDB, vm);
                    }

                    if (command == cli::PAYMENT_PROOF_VERIFY)
                    {
                        return VerifyPaymentProof(vm);
                    }

                    if (command == cli::WALLET_ADDRESS_LIST)
                    {
                        return ShowAddressList(walletDB);
                    }

                    io::Address receiverAddr;
                    Amount amount = 0;
                    Amount fee = 0;
                    WalletID receiverWalletID(Zero);
                    bool isTxInitiator = command == cli::SEND;
                    //bool isAssetEmit = command == cli::ASSET_EMIT;
                    //bool isAssetSend = command == cli::ASSET_SEND;
                    //bool isAssetBurn = command == cli::ASSET_BURN;
                    if (isTxInitiator && !LoadBaseParamsForTX(vm, amount, fee, receiverWalletID, isFork1))
                    {
                        return -1;
                    }

                    bool is_server = command == cli::LISTEN || vm.count(cli::LISTEN);

                    boost::optional<TxID> currentTxID;
                    auto txCompleteAction = [&currentTxID](const TxID& txID)
                    {
                        if (currentTxID.is_initialized() && currentTxID.get() != txID)
                        {
                            return;
                        }
                        io::Reactor::get_Current().stop();
                    };

                    Wallet wallet{ walletDB, is_server ? Wallet::TxCompletedAction() : txCompleteAction,
                                            !coldWallet ? Wallet::UpdateCompletedAction() : []() {io::Reactor::get_Current().stop(); } };
                    {
                        wallet::AsyncContextHolder holder(wallet);
                        if (!coldWallet)
                        {
                            if (vm.count(cli::NODE_ADDR) == 0)
                            {
                                LOG_ERROR() << "node address should be specified";
                                return -1;
                            }

                            string nodeURI = vm[cli::NODE_ADDR].as<string>();
                            io::Address nodeAddress;
                            if (!nodeAddress.resolve(nodeURI.c_str()))
                            {
                                LOG_ERROR() << "unable to resolve node address: " << nodeURI;
                                return -1;
                            }

                            auto nnet = make_shared<proto::FlyClient::NetworkStd>(wallet);
                            nnet->m_Cfg.m_PollPeriod_ms = vm[cli::NODE_POLL_PERIOD].as<Nonnegative<uint32_t>>().value;
                            if (nnet->m_Cfg.m_PollPeriod_ms)
                            {
                              LOG_INFO() << "Node poll period = " << nnet->m_Cfg.m_PollPeriod_ms << " ms";
                              uint32_t timeout_ms = std::max(Rules::get().DA.DTarget_s * 1000, nnet->m_Cfg.m_PollPeriod_ms);
                                if (timeout_ms != nnet->m_Cfg.m_PollPeriod_ms)
                                {
                                    LOG_INFO() << "Node poll period has been automatically rounded up to block rate: " << timeout_ms << " ms";
                                }
                            }
                            uint32_t responceTime_s = Rules::get().DA.DTarget_s * wallet::kDefaultTxResponseTime;
                            if (nnet->m_Cfg.m_PollPeriod_ms >= responceTime_s * 1000)
                            {
                              LOG_WARNING() << "The \"--node_poll_period\" parameter set to more than " << uint32_t(responceTime_s / 3600) << " hours may cause transaction problems.";

                            }
                            nnet->m_Cfg.m_vNodes.push_back(nodeAddress);
                            nnet->Connect();
                            wallet.AddMessageEndpoint(make_shared<WalletNetworkViaBbs>(wallet, nnet, walletDB));
                            wallet.SetNodeEndpoint(nnet);
                        }
                        else
                        {
                            wallet.AddMessageEndpoint(make_shared<ColdWalletMessageEndpoint>(wallet, walletDB));
                        }

                        if (!btcOptions.m_userName.empty() && !btcOptions.m_pass.empty())
                        {
                            btcOptions.m_feeRate = vm[cli::SWAP_FEERATE].as<Positive<Amount>>().value;

                            if (vm.count(cli::BTC_CONFIRMATIONS) > 0)
                            {
                                btcOptions.m_confirmations = vm[cli::BTC_CONFIRMATIONS].as<Positive<uint16_t>>().value;
                            }

                            if (vm.count(cli::BTC_LOCK_TIME) > 0)
                            {
                                btcOptions.m_lockTimeInBlocks = vm[cli::BTC_LOCK_TIME].as<Positive<uint32_t>>().value;
                            }

                            wallet.initBitcoin(io::Reactor::get_Current(), btcOptions);
                        }

                        if (!ltcOptions.m_userName.empty() && !ltcOptions.m_pass.empty())
                        {
                            ltcOptions.m_feeRate = vm[cli::SWAP_FEERATE].as<Positive<Amount>>().value;

                            if (vm.count(cli::LTC_CONFIRMATIONS) > 0)
                            {
                                ltcOptions.m_confirmations = vm[cli::LTC_CONFIRMATIONS].as<Positive<uint16_t>>().value;
                            }

                            if (vm.count(cli::LTC_LOCK_TIME) > 0)
                            {
                                ltcOptions.m_lockTimeInBlocks = vm[cli::LTC_LOCK_TIME].as<Positive<uint32_t>>().value;
                            }
                            wallet.initLitecoin(io::Reactor::get_Current(), ltcOptions);
                        }

                        if (command == cli::SWAP_INIT || command == cli::SWAP_LISTEN)
                        {
                            wallet::AtomicSwapCoin swapCoin = wallet::AtomicSwapCoin::Bitcoin;

                            if (vm.count(cli::SWAP_COIN) > 0)
                            {
                                swapCoin = wallet::from_string(vm[cli::SWAP_COIN].as<string>());

                                if (swapCoin == wallet::AtomicSwapCoin::Unknown)
                                {
                                    LOG_ERROR() << "Unknown coin for swap";
                                    return -1;
                                }
                            }

                            if (swapCoin == wallet::AtomicSwapCoin::Bitcoin)
                            {
                                if (btcOptions.m_userName.empty() || btcOptions.m_pass.empty() || btcOptions.m_address.empty())
                                {
                                    LOG_ERROR() << "BTC node credentials should be provided";
                                    return -1;
                                }
                            }
                            else
                            {
                                if (ltcOptions.m_userName.empty() || ltcOptions.m_pass.empty() || ltcOptions.m_address.empty())
                                {
                                    LOG_ERROR() << "LTC node credentials should be provided";
                                    return -1;
                                }
                            }

                            if (vm.count(cli::SWAP_AMOUNT) == 0)
                            {
                                LOG_ERROR() << "swap amount is missing";
                                return -1;
                            }

                            Amount swapAmount = vm[cli::SWAP_AMOUNT].as<Positive<Amount>>().value;
                            bool isGrimmSide = (vm.count(cli::SWAP_GRIMM_SIDE) != 0);

                            if (command == cli::SWAP_INIT)
                            {
                                if (!LoadBaseParamsForTX(vm, amount, fee, receiverWalletID, isFork1))
                                {
                                    return -1;
                                }

                                if (vm.count(cli::SWAP_AMOUNT) == 0)
                                {
                                    LOG_ERROR() << "swap amount is missing";
                                    return -1;
                                }

                                WalletAddress senderAddress = CreateNewAddress(walletDB, "");

                                currentTxID = wallet.swap_coins(senderAddress.m_walletID, receiverWalletID,
                                    move(amount), move(fee), swapCoin, swapAmount, isGrimmSide);
                            }

                            if (command == cli::SWAP_LISTEN)
                            {
                                if (vm.count(cli::AMOUNT) == 0)
                                {
                                    LOG_ERROR() << "amount is missing";
                                    return false;
                                }

                                auto signedAmount = vm[cli::AMOUNT].as<Positive<double>>().value;

                                signedAmount *= Rules::Coin; // convert grimms to coins

                                amount = static_cast<ECC::Amount>(std::round(signedAmount));
                                if (amount == 0)
                                {
                                    LOG_ERROR() << "Unable to send zero coins";
                                    return false;
                                }

                                wallet.initSwapConditions(amount, swapAmount, swapCoin, isGrimmSide);
                            }
                        }

                        //if (isAssetEmit) {
                        //     WalletAddress senderAddress = CreateNewAddress(walletDB, "");
                        //     WalletAddress receiverAddress = CreateNewAddress(walletDB, "");
                        //     AssetCommand assetCommand;
                        //     uint64_t idx = 0;
                        //     if (vm.count(cli::ASSET_KID) == 0)
                        //     {
                        //         LOG_ERROR() << "asset_kid required";
                        //         return false;
                        //     }
                        //     idx = vm[cli::ASSET_KID].as<uint64_t>();
                        //     AssetID assetID = Zero;
                        //     LOG_INFO() << "Creating assets...";
                        //     currentTxID = wallet.handle_asset(senderAddress.m_walletID, receiverAddress.m_walletID, assetCommand, move(amount), idx, assetID, move(fee), command == cli::SEND, kDefaultTxLifetime, kDefaultTxResponseTime, {});

                         //  }

                        if (isTxInitiator)
                        {
                            WalletAddress senderAddress = CreateNewAddress(walletDB, "");
                            AssetCommand assetCommand;
                            uint64_t idx = 0;
                            AssetID assetID = Zero;

                           if (LoadAssetParamsForTX(vm, assetCommand, idx, assetID))
                           {
                               currentTxID = wallet.handle_asset(senderAddress.m_walletID, receiverWalletID, assetCommand, move(amount), idx, assetID, move(fee), command == cli::SEND, kDefaultTxLifetime, kDefaultTxResponseTime, {});
                           }
                           else
                           {
                               CoinIDList coinIDs = GetPreselectedCoinIDs(vm);
                               currentTxID = wallet.transfer_money(senderAddress.m_walletID, receiverWalletID, move(amount), move(fee), coinIDs, command == cli::SEND, kDefaultTxLifetime, kDefaultTxResponseTime, {}, true);
                           }
                        }

                        bool deleteTx = command == cli::DELETE_TX;
                        if (command == cli::CANCEL_TX || deleteTx)
                        {
                            auto txIdVec = from_hex(vm[cli::TX_ID].as<string>());
                            TxID txId;
                            std::copy_n(txIdVec.begin(), 16, txId.begin());
                            auto tx = walletDB->getTx(txId);

                            if (tx)
                            {
                                if (deleteTx)
                                {
                                    if (tx->canDelete())
                                    {
                                        wallet.delete_tx(txId);
                                        return 0;
                                    }
                                    else
                                    {
                                        LOG_ERROR() << "Transaction could not be deleted. Invalid transaction status.";
                                        return -1;
                                    }
                                }
                                else
                                {
                                    if (tx->canCancel())
                                    {
                                        currentTxID = txId;
                                        wallet.cancel_tx(txId);
                                    }
                                    else
                                    {
                                        LOG_ERROR() << "Transaction could not be cancelled. Invalid transaction status.";
                                        return -1;
                                    }
                                }
                            }
                            else
                            {
                                LOG_ERROR() << "Unknown transaction ID.";
                                return -1;
                            }
                        }

                        if (command == cli::WALLET_RESCAN)
                        {
                            wallet.Refresh();
                        }
                    }
                    io::Reactor::get_Current().run();
                }
            }
        }
        catch (const AddressExpiredException&)
        {
        }
        catch (const FailToStartSwapException&)
        {
        }
        catch (const po::invalid_option_value& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const NonnegativeOptionException& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const PositiveOptionException& e)
        {
            cout << e.what() << std::endl;
            return 0;
        }
        catch (const po::error& e)
        {
            LOG_ERROR() << e.what();
            printHelp(visibleOptions);
        }
        catch (const std::runtime_error& e)
        {
            LOG_ERROR() << e.what();
        }
    }
    catch (const std::exception& e)
    {
        std::cout << e.what() << std::endl;
    }

    return 0;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    return main_impl(argc, argv);
#else
    block_sigpipe();
    auto f = std::async(
        std::launch::async,
        [argc, argv]() -> int {
            // TODO: this hungs app on OSX
            //lock_signals_in_this_thread();
            int ret = main_impl(argc, argv);
            kill(0, SIGINT);
            return ret;
        }
    );

    wait_for_termination(0);

    if (reactor) reactor->stop();

    return f.get();
#endif
}