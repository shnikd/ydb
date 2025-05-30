#include "service_table.h"
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/base/flow_control.h>

#include "rpc_kqp_base.h"
#include "service_table.h"

#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/core/base/appdata.h>
#include <ydb/library/ydb_issue/issue_helpers.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/kqp/opt/kqp_query_plan.h>

#include <ydb/library/services/services.pb.h>
#include <ydb/core/protos/ydb_table_impl.pb.h>
#include <ydb/core/ydb_convert/ydb_convert.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>

#include <ydb/core/protos/stream.pb.h>

namespace NKikimr {
namespace NGRpcService {

namespace {

using namespace NActors;
using namespace Ydb;
using namespace Ydb::Table;
using namespace NKqp;

struct TParseRequestError {
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;

    TParseRequestError()
        : Status(Ydb::StatusIds::INTERNAL_ERROR)
        , Issues({MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
            "Unexpected error while parsing request.")}) {}

    TParseRequestError(const Ydb::StatusIds::StatusCode& status, const NYql::TIssues& issues)
        : Status(status)
        , Issues(issues) {}
};

bool NeedReportStats(const Ydb::Table::ExecuteScanQueryRequest& req) {
    switch (req.mode()) {
        case ExecuteScanQueryRequest_Mode_MODE_UNSPECIFIED:
            return false;

        case ExecuteScanQueryRequest_Mode_MODE_EXPLAIN:
            return true;

        case ExecuteScanQueryRequest_Mode_MODE_EXEC:
            switch (req.collect_stats()) {
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_BASIC:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
                    return true;

                default:
                    break;
            }

            return false;
        default:
            return false;
    }
}

bool NeedReportPlan(const Ydb::Table::ExecuteScanQueryRequest& req) {
    switch (req.mode()) {
        case ExecuteScanQueryRequest_Mode_MODE_UNSPECIFIED:
            return false;

        case ExecuteScanQueryRequest_Mode_MODE_EXPLAIN:
            return true;

        case ExecuteScanQueryRequest_Mode_MODE_EXEC:
            switch (req.collect_stats()) {
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
                    return true;

                default:
                    break;
            }

            return false;

        default:
            return false;
    }
}

bool NeedCollectDiagnostics(const Ydb::Table::ExecuteScanQueryRequest& req) {
    switch (req.mode()) {
        case ExecuteScanQueryRequest_Mode_MODE_EXPLAIN:
            return true;

        case ExecuteScanQueryRequest_Mode_MODE_EXEC:
            switch (req.collect_stats()) {
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
                case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
                    return true;
                default:
                    break;
            }

            return false;

        default:
            return false;
    }
}

bool CheckRequest(const Ydb::Table::ExecuteScanQueryRequest& req, TParseRequestError& error)
{
    switch (req.mode()) {
        case Ydb::Table::ExecuteScanQueryRequest::MODE_EXEC:
            break;
        case Ydb::Table::ExecuteScanQueryRequest::MODE_EXPLAIN:
            break;
        default: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Unexpected query mode"));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }
    }

    auto& query = req.query();
    switch (query.query_case()) {
        case Ydb::Table::Query::kYqlText: {
            NYql::TIssues issues;
            if (!CheckQuery(query.yql_text(), issues)) {
                error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
                return false;
            }
            break;
        }

        case Ydb::Table::Query::kId: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
                "Specifying query by ID not supported in scan execution."));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }

        default: {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Unexpected query option"));
            error = TParseRequestError(Ydb::StatusIds::BAD_REQUEST, issues);
            return false;
        }
    }

    return true;
}

using TEvStreamExecuteScanQueryRequest = TGrpcRequestNoOperationCall<Ydb::Table::ExecuteScanQueryRequest,
    Ydb::Table::ExecuteScanQueryPartialResponse>;

class TStreamExecuteScanQueryRPC : public TActorBootstrapped<TStreamExecuteScanQueryRPC> {
private:
    enum EWakeupTag : ui64 {
        ClientLostTag = 1,
        TimeoutTag = 2
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::GRPC_STREAM_REQ;
    }

    TStreamExecuteScanQueryRPC(TEvStreamExecuteScanQueryRequest* request, ui64 rpcBufferSize)
        : Request_(request)
        , FlowControl_(rpcBufferSize) {}

    void Bootstrap(const TActorContext &ctx) {
        this->Become(&TStreamExecuteScanQueryRPC::StateWork);

        const auto& cfg = AppData(ctx)->StreamingConfig.GetOutputStreamConfig();

        InactiveClientTimeout_ = TDuration::FromValue(cfg.GetInactiveClientTimeout());
        if (InactiveClientTimeout_) {
            SetTimeoutTimer(InactiveClientTimeout_, ctx);
        }

        LastDataStreamTimestamp_ = TAppData::TimeProvider->Now();

        auto selfId = this->SelfId();
        auto as = TActivationContext::ActorSystem();

        Request_->SetFinishAction([selfId, as]() {
            as->Send(selfId, new TEvents::TEvWakeup(EWakeupTag::ClientLostTag));
        });

        Request_->SetStreamingNotify([selfId, as](size_t left) {
            as->Send(selfId, new TRpcServices::TEvGrpcNextReply(left));
        });

        Proceed(ctx);
    }

private:
    void StateWork(TAutoPtr<IEventHandle>& ev) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvWakeup, Handle);
            HFunc(TRpcServices::TEvGrpcNextReply, Handle);
            HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle);
            HFunc(NKqp::TEvKqp::TEvAbortExecution, Handle);
            HFunc(NKqp::TEvKqpExecuter::TEvStreamData, Handle);
            default: {
                auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, TStringBuilder()
                    << "Unexpected event received in TStreamExecuteScanQueryRPC::StateWork: " << ev->GetTypeRewrite());
                return ReplyFinishStream(Ydb::StatusIds::INTERNAL_ERROR, issue);
            }
        }
    }

    void Proceed(const TActorContext &ctx) {
        const auto req = Request_->GetProtoRequest();
        const auto traceId = Request_->GetTraceId();

        TParseRequestError parseError;
        if (!CheckRequest(*req, parseError)) {
            return ReplyFinishStream(parseError.Status, parseError.Issues);
        }

        auto action = (req->mode() == Ydb::Table::ExecuteScanQueryRequest::MODE_EXEC)
            ? NKikimrKqp::QUERY_ACTION_EXECUTE
            : NKikimrKqp::QUERY_ACTION_EXPLAIN;

        auto text = req->query().yql_text();
        auto ev = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>(
            action,
            NKikimrKqp::QUERY_TYPE_SQL_SCAN,
            SelfId(),
            Request_,
            TString(), //sessionId
            std::move(text),
            TString(), //queryId,
            nullptr, //tx_control
            &req->parameters(),
            req->collect_stats(),
            nullptr, // query_cache_policy
            nullptr
        );

        ev->Record.MutableRequest()->SetCollectDiagnostics(NeedCollectDiagnostics(*req));

        if (!ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), ev.Release())) {
            NYql::TIssues issues;
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Internal error"));
            ReplyFinishStream(Ydb::StatusIds::INTERNAL_ERROR, issues);
        }
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        switch ((EWakeupTag) ev->Get()->Tag) {
            case EWakeupTag::ClientLostTag:
                return HandleClientLost(ctx);
            case EWakeupTag::TimeoutTag:
                return HandleTimeout(ctx);
        }
    }

    void Handle(TRpcServices::TEvGrpcNextReply::TPtr& ev, const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " NextReply"
            << ", left: " << ev->Get()->LeftInQueue
            << ", queue: " << FlowControl_.QueueSize()
            << ", inflight bytes: " << FlowControl_.InflightBytes()
            << ", limit bytes: " << FlowControl_.InflightLimitBytes());

        while (FlowControl_.QueueSize() > ev->Get()->LeftInQueue) {
            FlowControl_.PopResponse();
        }

        LastDataStreamTimestamp_ = TAppData::TimeProvider->Now();

        const i64 freeSpaceBytes = FlowControl_.FreeSpaceBytes();
        if (freeSpaceBytes > 0 && LastSeqNo_ && AckedFreeSpaceBytes_ <= 0) {
            LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Send stream data ack"
                << ", seqNo: " << *LastSeqNo_
                << ", freeSpace: " << freeSpaceBytes
                << ", to: " << ExecuterActorId_);

            // scan query has single result set, so it's ok to put zero as channelId here.
            auto resp = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>(*LastSeqNo_, 0);
            resp->Record.SetFreeSpace(freeSpaceBytes);
            ctx.Send(ExecuterActorId_, resp.Release());
            AckedFreeSpaceBytes_ = freeSpaceBytes;
        }
    }

    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext&) {
        auto& record = ev->Get()->Record;

        NYql::TIssues issues;
        const auto& issueMessage = record.GetResponse().GetQueryIssues();
        NYql::IssuesFromMessage(issueMessage, issues);

        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            Request_->SetRuHeader(record.GetConsumedRu());

            Ydb::Table::ExecuteScanQueryPartialResponse response;
            TString out;
            auto& kqpResponse = record.GetResponse();
            response.set_status(Ydb::StatusIds::SUCCESS);

            bool reportStats = NeedReportStats(*Request_->GetProtoRequest());
            bool reportPlan = reportStats && NeedReportPlan(*Request_->GetProtoRequest());
            bool collectDiagnostics = NeedCollectDiagnostics(*Request_->GetProtoRequest());

            if (reportStats) {
                if (kqpResponse.HasQueryStats()) {

                    record.MutableResponse()->SetQueryPlan(reportPlan
                        ? SerializeAnalyzePlan(kqpResponse.GetQueryStats())
                        : "");

                    FillQueryStats(*response.mutable_result()->mutable_query_stats(), kqpResponse);
                } else if (reportPlan) {
                    response.mutable_result()->mutable_query_stats()->set_query_plan(kqpResponse.GetQueryPlan());
                }

                if (reportPlan) {
                    response.mutable_result()->mutable_query_stats()->set_query_ast(kqpResponse.GetQueryAst());
                }

                if (collectDiagnostics) {
                    response.mutable_result()->mutable_query_stats()->set_query_meta(kqpResponse.GetQueryDiagnostics());
                }

                Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
                Request_->SendSerializedResult(std::move(out), record.GetYdbStatus());
            }
        }
        ReplyFinishStream(record.GetYdbStatus(), issues);
    }

    void Handle(NKqp::TEvKqp::TEvAbortExecution::TPtr& ev, const TActorContext& ctx) {

        auto& record = ev->Get()->Record;
        NYql::TIssues issues = ev->Get()->GetIssues();

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Got abort execution event, from: "
            << ev->Sender << ", code: " << NYql::NDqProto::StatusIds::StatusCode_Name(record.GetStatusCode())
            << ", message: " << issues.ToOneLineString());

        ReplyFinishStream(NYql::NDq::DqStatusToYdbStatus(record.GetStatusCode()), issues);
    }

    void Handle(NKqp::TEvKqpExecuter::TEvStreamData::TPtr& ev, const TActorContext& ctx) {
        if (!ExecuterActorId_) {
            ExecuterActorId_ = ev->Sender;
        }

        auto& evRecord = ev->Get()->Record;

        Ydb::Table::ExecuteScanQueryPartialResponse response;

        {
            response.set_status(StatusIds::SUCCESS);
            auto result = response.mutable_result();
            result->mutable_result_set()->Swap(evRecord.MutableResultSet());

            if (evRecord.HasVirtualTimestamp()) {
                auto snap = result->mutable_snapshot();
                auto ts = evRecord.GetVirtualTimestamp();
                snap->set_plan_step(ts.GetStep());
                snap->set_tx_id(ts.GetTxId());
            }
        }

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);

        FlowControl_.PushResponse(out.size());
        const i64 freeSpaceBytes = FlowControl_.FreeSpaceBytes();
        LastSeqNo_ = evRecord.GetSeqNo();
        AckedFreeSpaceBytes_ = freeSpaceBytes;

        Request_->SendSerializedResult(std::move(out), StatusIds::SUCCESS);

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Send stream data ack"
            << ", seqNo: " << evRecord.GetSeqNo()
            << ", freeSpace: " << freeSpaceBytes
            << ", to: " << ev->Sender
            << ", queue: " << FlowControl_.QueueSize());

        auto resp = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>(evRecord.GetSeqNo(), evRecord.GetChannelId());
        resp->Record.SetFreeSpace(freeSpaceBytes);

        ctx.Send(ev->Sender, resp.Release());
    }

private:
    void SetTimeoutTimer(TDuration timeout, const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, this->SelfId() << " Set stream timeout timer for " << timeout);

        auto *ev = new IEventHandle(this->SelfId(), this->SelfId(), new TEvents::TEvWakeup(EWakeupTag::TimeoutTag));
        TimeoutTimerCookieHolder_.Reset(ISchedulerCookie::Make2Way());
        CreateLongTimer(ctx, timeout, ev, 0, TimeoutTimerCookieHolder_.Get());
    }

    void HandleClientLost(const TActorContext& ctx) {
        LOG_WARN_S(ctx, NKikimrServices::RPC_REQUEST, "Client lost");

        // We must try to finish stream otherwise grpc will not free allocated memory
        // If stream already scheduled to be finished (ReplyFinishStream already called)
        // this call do nothing but Die will be called after reply to grpc
        auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR,
            "Client should not see this message, if so... may the force be with you");
        ReplyFinishStream(StatusIds::INTERNAL_ERROR, issue);
    }

    void HandleTimeout(const TActorContext& ctx) {
        TInstant now = TAppData::TimeProvider->Now();
        TDuration timeout;
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Got timeout event, InactiveClientTimeout: " << InactiveClientTimeout_
            << " GRpcResponsesSizeQueue: " << FlowControl_.QueueSize());

        if (InactiveClientTimeout_ && FlowControl_.QueueSize() > 0) {
            TDuration processTime = now - LastDataStreamTimestamp_;
            if (processTime >= InactiveClientTimeout_) {
                auto message = TStringBuilder() << this->SelfId() << " Client cannot process data in " << processTime
                   << " which exceeds client timeout " << InactiveClientTimeout_;
                LOG_WARN_S(ctx, NKikimrServices::RPC_REQUEST, message);

                if (ExecuterActorId_) {
                    auto timeoutEv = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDqProto::StatusIds::TIMEOUT, "Client timeout");

                    ctx.Send(ExecuterActorId_, timeoutEv.Release());
                }

                auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, message);
                return ReplyFinishStream(StatusIds::TIMEOUT, issue);
            }
            TDuration remain = InactiveClientTimeout_ - processTime;
            timeout = timeout ? Min(timeout, remain) : remain;
        }

        if (timeout) {
            SetTimeoutTimer(timeout, ctx);
        }
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status, const NYql::TIssue& issue) {
        google::protobuf::RepeatedPtrField<TYdbIssueMessageType> issuesMessage;
        NYql::IssueToMessage(issue, issuesMessage.Add());

        ReplyFinishStream(status, issuesMessage);
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status, const NYql::TIssues& issues) {
        google::protobuf::RepeatedPtrField<TYdbIssueMessageType> issuesMessage;
        for (auto& issue : issues) {
            auto item = issuesMessage.Add();
            NYql::IssueToMessage(issue, item);
        }

        ReplyFinishStream(status, issuesMessage);
    }

    void ReplyFinishStream(Ydb::StatusIds::StatusCode status,
        const google::protobuf::RepeatedPtrField<TYdbIssueMessageType>& message)
    {
        ALOG_INFO(NKikimrServices::RPC_REQUEST, "Finish grpc stream, status: "
            << Ydb::StatusIds::StatusCode_Name(status));

        // Skip sending empty result in case of success status - simplify client logic
        if (status != Ydb::StatusIds::SUCCESS) {
            TString out;
            Ydb::Table::ExecuteScanQueryPartialResponse response;
            response.set_status(status);
            response.mutable_issues()->CopyFrom(message);
            Y_PROTOBUF_SUPPRESS_NODISCARD response.SerializeToString(&out);
            Request_->SendSerializedResult(std::move(out), status);
        }

        Request_->FinishStream(status);
        this->PassAway();
    }

private:
    std::shared_ptr<TEvStreamExecuteScanQueryRequest> Request_;
    TRpcFlowControlState FlowControl_;
    TMaybe<ui64> LastSeqNo_;
    i64 AckedFreeSpaceBytes_ = 0;

    TDuration InactiveClientTimeout_;
    TInstant LastDataStreamTimestamp_;

    TSchedulerCookieHolder TimeoutTimerCookieHolder_;

    TActorId ExecuterActorId_;
};

} // namespace

template<>
template<>
IActor* TEvStreamExecuteScanQueryRequest::CreateRpcActor(IRequestNoOpCtx* msg, ui64 rpcBufferSize) {
    auto* req = dynamic_cast<TEvStreamExecuteScanQueryRequest*>(msg);
    Y_ABORT_UNLESS(req != nullptr, "Wrong using of TGRpcRequestWrapper");
    return new TStreamExecuteScanQueryRPC(req, rpcBufferSize);
}

void DoExecuteScanQueryRequest(std::unique_ptr<IRequestNoOpCtx> p, const IFacilityProvider& f) {
    auto actor = TEvStreamExecuteScanQueryRequest::CreateRpcActor(p.release(), f.GetChannelBufferSize());
    f.RegisterActor(actor);
}

} // namespace NGRpcService
} // namespace NKikimr
