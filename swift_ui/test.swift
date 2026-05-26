import SwiftUI
struct TestView: View {
    var body: some View {
        Text("Sidebar").navigationSplitViewColumnWidth(min: 80, ideal: 200, max: 250)
    }
}
