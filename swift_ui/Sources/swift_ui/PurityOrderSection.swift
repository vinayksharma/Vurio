import SwiftUI

struct PurityOrderSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    
    @State private var isCollapsed = true
    
    // Hardcoded for now. Would ideally fetch from config.
    @State private var rules = [
        "CAMERA_RAW",
        "EXIF_CAPTURE_DATE",
        "FILE_SIZE",
        "FILE_CREATION_DATE",
        "RESOLUTION"
    ]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: {
                withAnimation {
                    isCollapsed.toggle()
                }
            }) {
                HStack {
                    Text("Purity Order")
                        .font(.title2)
                        .textCase(.uppercase)
                        .fontWeight(.semibold)
                        .foregroundColor(.primary)
                    Spacer()
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(.secondary)
                }
                .padding()
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())
            
            if !isCollapsed {
                VStack(alignment: .leading, spacing: 10) {
                    Text("Reorder the rules below to determine how the system selects the 'representative' file in a group of duplicates. The topmost rule has the highest priority.")
                        .foregroundColor(.secondary)
                        .padding(.bottom, 8)
                    
                    List {
                        ForEach(rules, id: \.self) { rule in
                            HStack {
                                Image(systemName: "line.3.horizontal")
                                    .foregroundColor(.secondary)
                                Text(rule)
                            }
                            .padding(.vertical, 4)
                        }
                        .onMove(perform: move)
                    }
                    .frame(height: 160)
                    .cornerRadius(8)
                    .listStyle(PlainListStyle())
                    
                    HStack {
                        Spacer()
                        Button("Apply Purity Rules") {
                            // This is where ConfigurePurityOrder would be called
                        }
                        .vurioButtonStyle(tint: Theme.textAccent)
                    }
                }
                .padding()
            }
        }
        .background(Theme.surface)
        .cornerRadius(10)
        .shadow(color: Color.black.opacity(0.3), radius: 10, x: 0, y: 5)
    }
    
    func move(from source: IndexSet, to destination: Int) {
        rules.move(fromOffsets: source, toOffset: destination)
    }
}
