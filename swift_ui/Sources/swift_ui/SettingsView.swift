import SwiftUI

struct SettingsView: View {
    @StateObject private var viewModel = SettingsViewModel()
    @State private var configSectionCollapsed = true
    
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                
                VStack(alignment: .leading, spacing: 4) {
                    Text("Settings")
                        .font(.largeTitle)
                        .fontWeight(.bold)
                    Text("Adjust server configuration and manage how VURIO behaves.")
                        .foregroundColor(.secondary)
                }
                .padding(.bottom, 10)
                
                // Add MediaLocationsSection here if needed, but the user excluded it from this task since it already exists elsewhere in the Swift app.
                
                // UISettings
                VStack(alignment: .leading, spacing: 10) {
                    Text("UI Settings")
                        .font(.title2)
                        .fontWeight(.semibold)
                    
                    Picker("Theme", selection: .constant("os")) {
                        Text("OS Default").tag("os")
                        Text("Light").tag("light")
                        Text("Dark").tag("dark")
                    }
                    .pickerStyle(SegmentedPickerStyle())
                    .disabled(true) // Currently forced to Dark Mode natively
                }
                .padding()
                .background(Color(NSColor.controlBackgroundColor))
                .cornerRadius(10)
                
                PurityOrderSection(viewModel: viewModel)
                
                // Configuration Section
                VStack(alignment: .leading, spacing: 0) {
                    Button(action: {
                        withAnimation {
                            configSectionCollapsed.toggle()
                        }
                    }) {
                        HStack {
                            Text("Configuration")
                                .font(.title2)
                                .fontWeight(.semibold)
                                .foregroundColor(.primary)
                            Spacer()
                            Image(systemName: configSectionCollapsed ? "chevron.right" : "chevron.down")
                                .foregroundColor(.secondary)
                        }
                    }
                    .buttonStyle(PlainButtonStyle())
                    .padding()
                    
                    if !configSectionCollapsed {
                        VStack(alignment: .leading, spacing: 15) {
                            Text("Review and edit advanced options. Changes are validated before saving.")
                                .foregroundColor(.secondary)
                            
                            HStack {
                                Spacer()
                                Button("Reload") {
                                    Task { await viewModel.loadConfig() }
                                }
                                .disabled(viewModel.isAnyActionRunning)
                                
                                Button("Save Changes") {
                                    Task { await viewModel.saveConfiguration() }
                                }
                                .buttonStyle(BorderedProminentButtonStyle())
                                .disabled(viewModel.modifiedProperties.isEmpty || viewModel.isAnyActionRunning)
                            }
                            
                            if viewModel.isLoadingConfig {
                                ProgressView("Loading configuration...")
                            } else {
                                ForEach(Array(viewModel.config.keys.sorted()), id: \.self) { key in
                                    HStack {
                                        Text(key)
                                            .frame(width: 250, alignment: .leading)
                                        TextField("", text: Binding(
                                            get: { viewModel.config[key] ?? "" },
                                            set: { newValue in
                                                viewModel.config[key] = newValue
                                                viewModel.modifiedProperties.insert(key)
                                            }
                                        ))
                                        .textFieldStyle(RoundedBorderTextFieldStyle())
                                    }
                                }
                            }
                        }
                        .padding()
                    }
                }
                .background(Color(NSColor.controlBackgroundColor))
                .cornerRadius(10)
                
                AdvancedSection(viewModel: viewModel)
                
            }
            .padding(24)
        }
        .onAppear {
            Task {
                await viewModel.loadConfig()
            }
        }
    }
}
