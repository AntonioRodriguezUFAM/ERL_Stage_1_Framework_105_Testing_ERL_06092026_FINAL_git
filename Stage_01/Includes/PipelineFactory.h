// PipelineFactory.h


class PipelineFactory {
public:
    static std::unique_ptr<PipelineResources> create(const json& config) {
        auto res = std::make_unique<PipelineResources>();
        validateAll(config);  // Central validation
        res->agg = createAggregator(config);
        res->tm = createThreadManager();
        res->queues = createQueues(config);
        res->soc = createSoC(config, res->agg);
        res->powerMon = createLynsyn(config, res->agg, res->tm);  // Optional
        res->camera = createCamera(config, res->queues, res->agg);
        res->algorithm = createAlgorithm(config, res->queues, res->tm, res->agg);
        res->display = createDisplay(config, res->queues, res->agg);
        return res;
    }
private:
    static void validateAll(const json& config) { /* All validations here */ }
    // Factory methods for each component...
};