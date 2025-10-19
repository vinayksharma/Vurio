#include "config/unified_observable_config.hpp"
#include "config/config_manager_factory.hpp"
#include "database/database_manager.hpp"
#include "database/image_artifacts_ops.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include <Poco/Logger.h>
#include <iostream>

using namespace MediaDedup;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: quality_smoke <image_path>\n";
        return 2;
    }

    std::string img = argv[1];

    ConfigManagerConfig cfg;
    cfg.config_file_path = "config/config.yaml";
    cfg.enable_file_monitoring = false;
    auto manager = ConfigManagerFactory::createWithConfig(cfg);
    if (!manager || !manager->initialize())
    {
        std::cerr << "Failed to init config\n";
        return 1;
    }

    int inputSize = manager->getPropertyValue<int>("media.image.quality.onnx.inputSize", 224);
    int dim = manager->getPropertyValue<int>("media.image.quality.embeddingDim", 512);
    std::string model = manager->getPropertyValue<std::string>("media.image.quality.onnx.modelPath", "models/clip-RN50.onnx");

    DatabaseManager db("/tmp/quality_smoke.sqlite");
    if (!db.initialize())
    {
        std::cerr << "Failed to init DB\n";
        return 1;
    }

    // Ensure table exists for artifacts
    (void)ImageArtifactsOps::ensureTable(db);

    QualityPipelineConfig pcfg;
    pcfg.input_size = inputSize;
    pcfg.embedding_dim = dim;
    pcfg.model = model;

    bool ok = QualityPipeline::Run(img, pcfg, db);
    std::cout << (ok ? "OK" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
