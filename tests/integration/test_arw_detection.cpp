int main() { 
    std::string test_file = "/Volumes/truenas._smb._tcp.local/Images Aft 20190701/2019/2019-07-31/DSC01787.ARW";
    bool is_raw = TranscodingManager::isRawFile(test_file);
    std::cout << "ARW file detection: " << (is_raw ? "YES" : "NO") << std::endl;
    return 0;
}
