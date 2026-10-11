import SwiftUI

struct SettingsView: View {
    @StateObject private var viewModel = SettingsViewModel()
    @State private var configSectionCollapsed = true
    
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                
                MediaLocationsView()

                // CHAIN-3: recipes travel as files through here.
                RecipesSection()
                
                PurityOrderSection(viewModel: viewModel)
                
                

                // Configuration Section
                VStack(alignment: .leading, spacing: 0) {
                    Button(action: {
                        withAnimation {
                            configSectionCollapsed.toggle()
                        }
                    }) {
                        HStack(spacing: 12) {
                            Image(systemName: "gearshape.2.fill")
                                .font(.title2)
                                .foregroundColor(Theme.textPrimary)
                            Text("Configuration")
                                .font(.title2)
                                .textCase(.uppercase)
                                .fontWeight(.semibold)
                                .foregroundColor(Theme.textPrimary)
                            Spacer()
                            Image(systemName: configSectionCollapsed ? "chevron.right" : "chevron.down")
                                .foregroundColor(Theme.textSecondary)
                        }
                        .padding()
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    if !configSectionCollapsed {
                        VStack(alignment: .leading, spacing: 15) {
                            Text("Review and edit advanced options. Changes are validated before saving.")
                                .foregroundColor(Theme.textSecondary)
                            
                            HStack {
                                Spacer()
                                Button("Reload") {
                                    Task { await viewModel.loadConfig() }
                                }
                                .vurioButtonStyle(tint: Theme.textPrimary)
                                .disabled(viewModel.isAnyActionRunning)
                                
                                Button("Save Changes") {
                                    Task { await viewModel.saveConfiguration() }
                                }
                                .vurioButtonStyle(tint: Theme.textAccent)
                                .disabled(viewModel.modifiedProperties.isEmpty || viewModel.isAnyActionRunning)
                            }
                            
                            if viewModel.isLoadingConfig {
                                ProgressView("Loading configuration...")
                            } else {
                                ForEach(viewModel.groupedConfigKeys, id: \.0) { sectionName, keys in
                                    VStack(alignment: .leading, spacing: 10) {
                                        HStack(spacing: 8) {
                                            Image(systemName: iconForSection(sectionName))
                                                .foregroundColor(Theme.textSecondary)
                                            Text(sectionName)
                                                .font(.headline)
                                                .textCase(.uppercase)
                                                .foregroundColor(Theme.textSecondary)
                                        }
                                        .padding(.top, 10)
                                        
                                        ForEach(keys, id: \.self) { key in
                                            ConfigRowView(key: key, viewModel: viewModel)
                                        }
                                        
                                        if sectionName == "Duplicates" {
                                            MatchScoreSliderSection(viewModel: viewModel)
                                        }
                                        
                                        if sectionName == "Media - Face" {
                                            FaceCutoffSliderSection(viewModel: viewModel)
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
                .background(Theme.surface)
                .cornerRadius(10)
                .shadow(color: Color.black.opacity(0.3), radius: 10, x: 0, y: 5)
                
                AdvancedSection(viewModel: viewModel)
                
            }
            .padding(24)
        }
        .onAppear {
            Task {
                await viewModel.loadConfig()
            }
        }
        .overlay(
            Group {
                if let message = viewModel.activeOperationMessage {
                    ZStack {
                        Color.black.opacity(0.4)
                            .edgesIgnoringSafeArea(.all)
                        
                        VStack(spacing: 16) {
                            ProgressView()
                                .controlSize(.regular)
                            Text(message)
                                .font(.headline)
                            Text("Please wait...")
                                .font(.subheadline)
                                .foregroundColor(Theme.textSecondary)
                        }
                        .padding(30)
                        .background(Theme.background)
                        .cornerRadius(12)
                        .shadow(radius: 10)
                    }
                }
            }
        )
        .disabled(viewModel.activeOperationMessage != nil)
    }
}

extension SettingsView {
    func iconForSection(_ section: String) -> String {
        let s = section.lowercased()
        if s.contains("duplicates") { return "doc.on.doc" }
        if s.contains("media") { return "folder.fill" }
        if s.contains("tpm") { return "cpu" }
        if s.contains("scanner") { return "magnifyingglass" }
        if s.contains("processor") { return "cpu.fill" }
        return "gearshape"
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
                    .foregroundColor(Theme.textSecondary)
            }
            .buttonStyle(PlainButtonStyle())
            .popover(isPresented: $showPopover, arrowEdge: .trailing) {
                ASTPopoverContent(textContent: viewModel.configLongDescriptions[key] ?? viewModel.configDescriptions[key] ?? "Configuration setting")
                    .fixedSize(horizontal: false, vertical: true)
                    .padding()
                    .frame(width: 300)
            }
            
            Text(viewModel.configDescriptions[key] ?? key)
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

struct ASTNode: Codable, Hashable {
    let type: String
    let text: String
}

struct ASTPopoverContent: View {
    let textContent: String
    
    var nodes: [ASTNode]? {
        guard let data = textContent.data(using: .utf8),
              let decoded = try? JSONDecoder().decode([ASTNode].self, from: data) else {
            return nil
        }
        return decoded
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let parsedNodes = nodes, !parsedNodes.isEmpty {
                ForEach(parsedNodes, id: \.self) { node in
                    if node.type == "headline" {
                        Text(.init(node.text))
                            .font(.headline)
                            .textCase(.uppercase)
                    } else {
                        Text(.init(node.text))
                    }
                }
            } else {
                Text(.init(textContent))
            }
        }
    }
}
