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
                

                
                PurityOrderSection(viewModel: viewModel)
                
                MatchScoreSliderSection(viewModel: viewModel)
                
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
                        .contentShape(Rectangle())
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
                                ForEach(viewModel.groupedConfigKeys, id: \.0) { sectionName, keys in
                                    VStack(alignment: .leading, spacing: 10) {
                                        Text(sectionName)
                                            .font(.headline)
                                            .foregroundColor(.secondary)
                                            .padding(.top, 10)
                                        
                                        ForEach(keys, id: \.self) { key in
                                            ConfigRowView(key: key, viewModel: viewModel)
                                        }
                                        
                                        Divider()
                                            .padding(.vertical, 5)
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

struct ConfigRowView: View {
    let key: String
    @ObservedObject var viewModel: SettingsViewModel
    @State private var showPopover = false
    
    var body: some View {
        HStack {
            Button(action: {
                showPopover.toggle()
            }) {
                Image(systemName: "info.circle")
                    .foregroundColor(.secondary)
            }
            .buttonStyle(PlainButtonStyle())
            .popover(isPresented: $showPopover, arrowEdge: .trailing) {
                Text(viewModel.configLongDescriptions[key] ?? viewModel.configDescriptions[key] ?? "Configuration setting")
                    .fixedSize(horizontal: false, vertical: true)
                    .padding()
                    .frame(width: 300)
            }
            
            Text(key)
                .frame(width: 350, alignment: .leading)
                .help(viewModel.configDescriptions[key] ?? "")
            
            let originalValue = viewModel.originalConfig[key] ?? ""
            if let options = viewModel.configOptions[key], !options.isEmpty {
                Picker("", selection: Binding(
                    get: { viewModel.config[key] ?? "" },
                    set: { newValue in
                        viewModel.config[key] = newValue
                        viewModel.modifiedProperties.insert(key)
                    }
                )) {
                    ForEach(options, id: \.self) { option in
                        Text(option).tag(option)
                    }
                }
                .labelsHidden()
            } else if originalValue == "true" || originalValue == "false" {
                Toggle("", isOn: Binding(
                    get: { (viewModel.config[key] ?? "") == "true" },
                    set: { newValue in
                        viewModel.config[key] = newValue ? "true" : "false"
                        viewModel.modifiedProperties.insert(key)
                    }
                ))
                .labelsHidden()
            } else if Double(originalValue) != nil || Int(originalValue) != nil {
                TextField("", text: Binding(
                    get: { viewModel.config[key] ?? "" },
                    set: { newValue in
                        let filtered = newValue.filter { "0123456789.-".contains($0) }
                        viewModel.config[key] = filtered
                        viewModel.modifiedProperties.insert(key)
                    }
                ))
                .textFieldStyle(RoundedBorderTextFieldStyle())
            } else {
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

